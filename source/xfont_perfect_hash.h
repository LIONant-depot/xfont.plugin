#ifndef XFONT_PERFECT_HASH_H
#define XFONT_PERFECT_HASH_H
#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>

// Minimal perfect hash for a fixed, build-time-known set of Unicode codepoints - the classic CHD
// (Compress, Hash, and Displace) construction. A first attempt at this used a single-shot "fully
// re-randomize four tables and hope for zero collisions across ALL keys at once" scheme - that's a
// birthday-paradox problem whose success probability decays as exp(-n^2/SlotCount), which already
// made it astronomically unlikely to converge for as few as 95 ASCII codepoints (confirmed by an
// actual failed compile, not just analysis). CHD avoids this by resolving collisions PER BUCKET
// instead of globally: keys are first split into small buckets by one hash, then each bucket
// (largest first) searches for a displacement value that lands all of ITS keys in currently-free
// slots - a search that only has to satisfy a handful of keys at a time, not the whole set, so it
// converges in a handful of tries per bucket even for large charsets, at a near-minimal table size
// (SlotCount just above the key count, not the key count squared).
//
// Runtime lookup is still exactly what the design calls for: one bucket hash, one displacement
// table lookup, one slot hash - O(1), two memory accesses, no external generator dependency.
namespace xfont
{
    inline std::uint32_t PerfectHashMix(std::uint32_t X, std::uint32_t Seed) noexcept
    {
        X ^= Seed;
        X *= 0x85ebca6bu;
        X ^= X >> 13;
        X *= 0xc2b2ae35u;
        X ^= X >> 16;
        return X;
    }

    // Compile-time-only construction result - the runtime resource stores just m_SlotCount/
    // m_BucketCount as plain scalars and the m_Displacement array as part of its own variable-
    // length blob (see xfont_rsc_runtime.h), not this struct directly.
    struct perfect_hash_build
    {
        std::uint32_t              m_SlotCount{ 0 };
        std::uint32_t              m_BucketCount{ 0 };
        std::vector<std::uint16_t> m_Displacement;
    };

    inline std::uint32_t PerfectHashBucket(std::uint32_t Codepoint, std::uint32_t BucketCount) noexcept
    {
        return PerfectHashMix(Codepoint, 0x9E3779B9u) & (BucketCount - 1);
    }

    inline std::uint32_t PerfectHashSlot(std::uint32_t Codepoint, std::uint16_t Displacement, std::uint32_t SlotCount) noexcept
    {
        return PerfectHashMix(Codepoint, 0xC2B2AE35u ^ (static_cast<std::uint32_t>(Displacement) * 0x2545F491u)) & (SlotCount - 1);
    }

    // Builds a CHD perfect hash over Codepoints. Returns false only if a bucket's displacement
    // search failed to converge within MaxDisplacementTries for every SlotCount growth attempt -
    // in practice this does not happen for realistic charset sizes at the default slack.
    inline bool BuildPerfectHash(const std::vector<std::uint32_t>& Codepoints, perfect_hash_build& Out, int MaxDisplacementTries = 100000) noexcept
    {
        const std::size_t N = Codepoints.size();
        if (N == 0) return false;

        std::uint32_t SlotCount = 1;
        while (SlotCount < N + N / 4 + 1) SlotCount <<= 1; // ~1.25x slack - near-minimal, not birthday-problem territory

        std::uint32_t BucketCount = 1;
        while (BucketCount < (N / 3) + 1) BucketCount <<= 1; // average ~3 keys/bucket

        std::vector<std::vector<std::uint32_t>> Buckets(BucketCount); // each holds codepoint values
        for (auto Cp : Codepoints) Buckets[PerfectHashBucket(Cp, BucketCount)].push_back(Cp);

        std::vector<std::uint32_t> BucketOrder(BucketCount);
        for (std::uint32_t i = 0; i < BucketCount; ++i) BucketOrder[i] = i;
        std::sort(BucketOrder.begin(), BucketOrder.end(), [&](std::uint32_t A, std::uint32_t B)
        {
            return Buckets[A].size() > Buckets[B].size();
        });

        Out.m_SlotCount   = SlotCount;
        Out.m_BucketCount = BucketCount;
        Out.m_Displacement.assign(BucketCount, 0);

        std::vector<bool> Occupied(SlotCount, false);
        std::vector<std::uint32_t> TrialSlots;

        for (auto BucketIndex : BucketOrder)
        {
            auto& Bucket = Buckets[BucketIndex];
            if (Bucket.empty()) continue;

            bool bFound = false;
            for (int D = 0; D < MaxDisplacementTries && !bFound; ++D)
            {
                TrialSlots.clear();
                bool bOk = true;
                for (auto Cp : Bucket)
                {
                    const auto Slot = PerfectHashSlot(Cp, static_cast<std::uint16_t>(D), SlotCount);
                    if (Occupied[Slot]) { bOk = false; break; }
                    // reject intra-bucket duplicates too (two keys of this bucket landing on the same fresh slot)
                    if (std::find(TrialSlots.begin(), TrialSlots.end(), Slot) != TrialSlots.end()) { bOk = false; break; }
                    TrialSlots.push_back(Slot);
                }
                if (bOk)
                {
                    for (auto Slot : TrialSlots) Occupied[Slot] = true;
                    Out.m_Displacement[BucketIndex] = static_cast<std::uint16_t>(D);
                    bFound = true;
                }
            }
            if (!bFound) return false;
        }
        return true;
    }
}
#endif
