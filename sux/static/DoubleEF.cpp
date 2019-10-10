#include <iostream>
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <bitset>
#include <algorithm>
#include "DoubleEF.hpp"

namespace sux {

DoubleEF::DoubleEF(const std::vector<uint64_t>& cum_keys, const std::vector<uint64_t>& position) {
	assert(cum_keys.size() == position.size());
	num_buckets = cum_keys.size() - 1;

	bits_per_key_fixed_point = (uint64_t(1) << 20) * (position[num_buckets] / (double)cum_keys[num_buckets]);

	min_diff = std::numeric_limits<int64_t>::max() / 2;
	cum_keys_min_delta = std::numeric_limits<int64_t>::max() / 2;
	int64_t prev_bucket_bits = 0;
	for(size_t i = 1; i <= num_buckets; ++i) {
		const int64_t nkeys_delta = cum_keys[i] - cum_keys[i - 1];
		cum_keys_min_delta = min(cum_keys_min_delta, nkeys_delta);
		const int64_t bucket_bits = int64_t(position[i]) - int64_t(bits_per_key_fixed_point * cum_keys[i] >> 20);
		min_diff = min(min_diff, bucket_bits - prev_bucket_bits);
		prev_bucket_bits = bucket_bits;
	}

	u_position = int64_t(position[num_buckets]) - int64_t(bits_per_key_fixed_point * cum_keys[num_buckets] >> 20) - int64_t(num_buckets * min_diff) + 1;
	l_position = u_position / (num_buckets + 1) == 0 ? 0 : lambda(u_position / (num_buckets + 1));
	u_cum_keys = cum_keys[num_buckets] - num_buckets * cum_keys_min_delta + 1;
	l_cum_keys = u_cum_keys / (num_buckets + 1) == 0 ? 0 : lambda(u_cum_keys / (num_buckets + 1));
	assert(l_cum_keys * 2 + l_position <= 56); // To be able to perform a single unaligned read

#ifdef MORESTATS
	printf("Elias-Fano l (cumulative): %d\n", l_cum_keys);
	printf("Elias-Fano l (positions): %d\n", l_position);
	printf("Elias-Fano u (cumulative): %lld\n", u_cum_keys);
	printf("Elias-Fano u (positions): %lld\n", u_position);
#endif

	lower_bits_mask_cum_keys = (UINT64_C(1) << l_cum_keys) - 1;
	lower_bits_mask_position = (UINT64_C(1) << l_position) - 1;

	const uint64_t words_lower_bits = lower_bits_size_words();
	lower_bits = new uint64_t[words_lower_bits];
	const uint64_t words_cum_keys = cum_keys_size_words();
	upper_bits_cum_keys = new uint64_t[words_cum_keys]();
	const uint64_t words_position = position_size_words();
	upper_bits_position = new uint64_t[words_position]();

	for(int64_t i = 0, cum_delta = 0, bit_delta = 0; i <= num_buckets; i++, cum_delta += cum_keys_min_delta, bit_delta += min_diff) {
		if (l_cum_keys != 0) set_bits(lower_bits, i * (l_cum_keys + l_position), l_cum_keys, (cum_keys[i] - cum_delta) & lower_bits_mask_cum_keys);
		set(upper_bits_cum_keys, ((cum_keys[i] - cum_delta) >> l_cum_keys) + i);
		const auto pval = int64_t(position[i]) - int64_t(bits_per_key_fixed_point * cum_keys[i] >> 20);
		if (l_position != 0) set_bits(lower_bits, i * (l_cum_keys + l_position) + l_cum_keys, l_position, (pval - bit_delta) & lower_bits_mask_position);
		set(upper_bits_position, ((pval - bit_delta) >> l_position) + i);
	}

	const uint64_t jump_words = jump_size_words();
	jump = new uint64_t[jump_words];

	for(uint64_t i = 0, c = 0, last_super_q = 0; i < words_cum_keys; i++) {
		for(int b = 0; b < 64; b++) {
			if (upper_bits_cum_keys[i] & UINT64_C(1) << b) {
				if ((c & super_q_mask) == 0) jump[(c / super_q) * (super_q_size * 2)] = last_super_q = i * 64 + b;
				if ((c & q_mask) == 0) {
					const uint64_t offset = i * 64 + b - last_super_q;
					if (offset >= (1 << 16)) abort();
					((uint16_t *)(jump + (c / super_q) * (super_q_size * 2) + 2))[2 * ((c % super_q) / q)] = offset;
				}
				c++;
			}
		}
	}

	for(uint64_t i = 0, c = 0, last_super_q = 0; i < words_position; i++) {
		for(int b = 0; b < 64; b++) {
			if (upper_bits_position[i] & UINT64_C(1) << b) {
				if ((c & super_q_mask) == 0) jump[(c / super_q) * (super_q_size * 2) + 1] = last_super_q = i * 64 + b;
				if ((c & q_mask) == 0) {
					const uint64_t offset = i * 64 + b - last_super_q;
					if (offset >= (1 << 16)) abort();
					((uint16_t *)(jump + (c / super_q) * (super_q_size * 2) + 2))[2 * ((c % super_q) / q) + 1] = offset;
				}
				c++;
			}
		}
	}

#ifndef NDEBUG
	for(uint64_t i = 0; i < num_buckets; i++) {
		uint64_t x, x2, y;

		get(i, x, x2, y);
		assert(x == cum_keys[i]);
		assert(x2 == cum_keys[i + 1]);
		assert(y == position[i]);

		get(i, x, y);
		assert(x == cum_keys[i]);
		assert(y == position[i]);
	}
#endif
}

void DoubleEF::get(const uint64_t i, uint64_t& cum_keys, uint64_t& cum_keys_next, uint64_t& position) {
	const uint64_t pos_lower = i * (l_cum_keys + l_position);
	uint64_t lower;
	memcpy(&lower, (uint8_t *)lower_bits + pos_lower / 8, 8);
	lower >>= pos_lower % 8;

	const uint64_t jump_super_q = (i / super_q) * super_q_size * 2;
	const uint64_t jump_inside_super_q = (i % super_q) / q;
	const uint64_t jump_cum_keys = jump[jump_super_q] + ((uint16_t *)(jump + jump_super_q + 2))[2 * jump_inside_super_q];
	const uint64_t jump_position = jump[jump_super_q + 1] + ((uint16_t *)(jump + jump_super_q + 2))[2 * jump_inside_super_q + 1];

	uint64_t curr_word_cum_keys = jump_cum_keys / 64;
	uint64_t curr_word_position = jump_position / 64;
	uint64_t window_cum_keys = upper_bits_cum_keys[curr_word_cum_keys] & UINT64_C(-1) << jump_cum_keys % 64;
	uint64_t window_position = upper_bits_position[curr_word_position] & UINT64_C(-1) << jump_position % 64;
	uint64_t delta_cum_keys = i & q_mask;
	uint64_t delta_position = i & q_mask;

	for(uint64_t bit_count; (bit_count = nu(window_cum_keys)) <= delta_cum_keys; delta_cum_keys -= bit_count ) window_cum_keys = upper_bits_cum_keys[++curr_word_cum_keys];
	for(uint64_t bit_count; (bit_count = nu(window_position)) <= delta_position; delta_position -= bit_count ) window_position = upper_bits_position[++curr_word_position];

	const uint64_t select_cum_keys = select64(window_cum_keys, delta_cum_keys);
	const int64_t cum_delta = i * cum_keys_min_delta;
	cum_keys = ((curr_word_cum_keys * 64 + select_cum_keys - i) << l_cum_keys | (lower & lower_bits_mask_cum_keys)) + cum_delta;

	lower >>= l_cum_keys;
	const int64_t bit_delta = i * min_diff;
	position = ((curr_word_position * 64 + select64(window_position, delta_position) - i) << l_position | (lower & lower_bits_mask_position)) + bit_delta + int64_t(bits_per_key_fixed_point * cum_keys >> 20);

	window_cum_keys &= (-1ULL << select_cum_keys) << 1;
	while(window_cum_keys == 0) window_cum_keys = upper_bits_cum_keys[++curr_word_cum_keys];

	lower >>= l_position;
	cum_keys_next = ((curr_word_cum_keys * 64 + rho(window_cum_keys) - i - 1) << l_cum_keys | (lower & lower_bits_mask_cum_keys)) + cum_delta + cum_keys_min_delta;
}

void DoubleEF::get(const uint64_t i, uint64_t& cum_keys, uint64_t& position) {
	const uint64_t pos_lower = i * (l_cum_keys + l_position);
	uint64_t lower;
	memcpy(&lower, (uint8_t *)lower_bits + pos_lower / 8, 8);
	lower >>= pos_lower % 8;

	const uint64_t jump_super_q = (i / super_q) * super_q_size * 2;
	const uint64_t jump_inside_super_q = (i % super_q) / q;
	const uint64_t jump_cum_keys = jump[jump_super_q] + ((uint16_t *)(jump + jump_super_q + 2))[2 * jump_inside_super_q];
	const uint64_t jump_position = jump[jump_super_q + 1] + ((uint16_t *)(jump + jump_super_q + 2))[2 * jump_inside_super_q + 1];

	uint64_t curr_word_cum_keys = jump_cum_keys / 64;
	uint64_t curr_word_position = jump_position / 64;
	uint64_t window_cum_keys = upper_bits_cum_keys[curr_word_cum_keys] & UINT64_C(-1) << jump_cum_keys % 64;
	uint64_t window_position = upper_bits_position[curr_word_position] & UINT64_C(-1) << jump_position % 64;
	uint64_t delta_cum_keys = i & q_mask;
	uint64_t delta_position = i & q_mask;

	for(uint64_t bit_count; (bit_count = nu(window_cum_keys)) <= delta_cum_keys; delta_cum_keys -= bit_count ) window_cum_keys = upper_bits_cum_keys[++curr_word_cum_keys];
	for(uint64_t bit_count; (bit_count = nu(window_position)) <= delta_position; delta_position -= bit_count ) window_position = upper_bits_position[++curr_word_position];

	const uint64_t select_cum_keys = select64(window_cum_keys, delta_cum_keys);
	const size_t cum_delta = i * cum_keys_min_delta;
	cum_keys = ((curr_word_cum_keys * 64 + select_cum_keys - i) << l_cum_keys | (lower & lower_bits_mask_cum_keys)) + cum_delta;

	lower >>= l_cum_keys;
	const int64_t bit_delta = i * min_diff;
	position = ((curr_word_position * 64 + select64(window_position, delta_position) - i) << l_position | (lower & lower_bits_mask_position)) + bit_delta + int64_t(bits_per_key_fixed_point * cum_keys >> 20);
}

uint64_t DoubleEF::bit_count_cum_keys() {
	return (num_buckets + 1) * l_cum_keys + num_buckets + 1 + (u_cum_keys >> l_cum_keys) + jump_size_words() / 2;
}

uint64_t DoubleEF::bit_count_position() {
	return (num_buckets + 1) * l_position + num_buckets + 1 + (u_position >> l_position) + jump_size_words() / 2;
}


DoubleEF::~DoubleEF() {
	delete [] upper_bits_position;
	delete [] upper_bits_cum_keys;
	delete [] lower_bits;
	delete [] jump;
}

int DoubleEF::dump(FILE* fp) const {
	fwrite(&num_buckets, sizeof(num_buckets), (size_t)1, fp);
	fwrite(&u_cum_keys, sizeof(u_cum_keys), (size_t)1, fp);
	fwrite(&u_position, sizeof(u_position), (size_t)1, fp);
	fwrite(&cum_keys_min_delta, sizeof(cum_keys_min_delta), (size_t)1, fp);
	fwrite(&min_diff, sizeof(min_diff), (size_t)1, fp);
	fwrite(&bits_per_key_fixed_point, sizeof(bits_per_key_fixed_point), (size_t)1, fp);

	const uint64_t words_lower_bits = lower_bits_size_words();
	fwrite(lower_bits, sizeof(uint64_t), (size_t)words_lower_bits, fp);
	const uint64_t words_cum_keys = cum_keys_size_words();
	fwrite(upper_bits_cum_keys, sizeof(uint64_t), (size_t)words_cum_keys, fp);
	const uint64_t words_position = position_size_words();
	fwrite(upper_bits_position, sizeof(uint64_t), (size_t)words_position, fp);

	const uint64_t jump_words = jump_size_words();
	fwrite(jump, sizeof(uint64_t), (size_t)jump_words, fp);

	return 1;
}

void DoubleEF::load(FILE* fp) {
	fread(&num_buckets, sizeof(num_buckets), (size_t)1, fp);
	fread(&u_cum_keys, sizeof(u_cum_keys), (size_t)1, fp);
	fread(&u_position, sizeof(u_position), (size_t)1, fp);
	fread(&cum_keys_min_delta, sizeof(cum_keys_min_delta), (size_t)1, fp);
	fread(&min_diff, sizeof(min_diff), (size_t)1, fp);
	fread(&bits_per_key_fixed_point, sizeof(bits_per_key_fixed_point), (size_t)1, fp);

	l_position = u_position / (num_buckets + 1) == 0 ? 0 : lambda(u_position / (num_buckets + 1));
	l_cum_keys = u_cum_keys / (num_buckets + 1) == 0 ? 0 : lambda(u_cum_keys / (num_buckets + 1));
	assert(l_cum_keys * 2 + l_position <= 56);

	lower_bits_mask_cum_keys = (UINT64_C(1) << l_cum_keys) - 1;
	lower_bits_mask_position = (UINT64_C(1) << l_position) - 1;

	const uint64_t words_lower_bits = lower_bits_size_words();
	lower_bits = new uint64_t[words_lower_bits];
	fread(lower_bits, sizeof(uint64_t), (size_t)words_lower_bits, fp);
	const uint64_t words_cum_keys = cum_keys_size_words();
	upper_bits_cum_keys = new uint64_t[words_cum_keys]();
	fread(upper_bits_cum_keys, sizeof(uint64_t), (size_t)words_cum_keys, fp);
	const uint64_t words_position = position_size_words();
	upper_bits_position = new uint64_t[words_position]();
	fread(upper_bits_position, sizeof(uint64_t), (size_t)words_position, fp);

	const uint64_t jump_words = jump_size_words();
	jump = new uint64_t[jump_words];
	fread(jump, sizeof(uint64_t), (size_t)jump_words, fp);
}

}