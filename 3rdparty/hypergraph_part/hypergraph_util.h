#ifndef HYPERGRAPH_UTIL_H
#define HYPERGRAPH_UTIL_H

#include <algorithm>
#include <stdexcept>

#define HYPER_ASSERT(x) do { if(!(x)) throw std::runtime_error("assertion failure: " #x); } while (0);

namespace HyperPart {
	template <typename T> int GetSize(const T& t) { return int(t.size()); }


	// From nextpnr
	struct DeterministicRNG
	{
		uint64_t rngstate;

		DeterministicRNG() : rngstate(0x3141592653589793) {}

		uint64_t rng64()
		{
			// xorshift64star
			// https://arxiv.org/abs/1402.6246

			uint64_t retval = rngstate * 0x2545F4914F6CDD1D;

			rngstate ^= rngstate >> 12;
			rngstate ^= rngstate << 25;
			rngstate ^= rngstate >> 27;

			return retval;
		}

		int rng() { return rng64() & 0x3fffffff; }

		int rng(int n)
		{
			HYPER_ASSERT(n > 0);

			// round up to power of 2
			int m = n - 1;
			m |= (m >> 1);
			m |= (m >> 2);
			m |= (m >> 4);
			m |= (m >> 8);
			m |= (m >> 16);
			m += 1;

			while (1) {
				int x = rng64() & (m - 1);
				if (x < n)
					return x;
			}
		}

		void rngseed(uint64_t seed)
		{
			rngstate = seed ? seed : 0x3141592653589793;
			for (int i = 0; i < 5; i++)
				rng64();
		}

		template <typename T> void shuffle(std::vector<T> &a)
		{
			for (size_t i = 0; i != a.size(); i++) {
				size_t j = i + rng(a.size() - i);
				if (j > i)
					std::swap(a[i], a[j]);
			}
		}

		template <typename T> void sorted_shuffle(std::vector<T> &a)
		{
			std::sort(a.begin(), a.end());
			shuffle(a);
		}
	};

}

#endif
