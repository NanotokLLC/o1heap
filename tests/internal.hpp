// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
// and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Copyright (c) 2020 UAVCAN Development Team
// Authors: Pavel Kirienko <pavel.kirienko@zubax.com>

#ifndef O1HEAP_TESTS_INTERNAL_HPP_INCLUDED
#define O1HEAP_TESTS_INTERNAL_HPP_INCLUDED

#include "catch.hpp"
#include "o1heap.h"
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>

/// Definitions that are not exposed by the library but that are needed for testing.
/// Please keep them in sync with the library by manually updating as necessary.
namespace internal
{
extern "C" {
auto isPowerOf2(const std::size_t x) -> bool;
auto log2Floor(const std::size_t x) -> std::uint8_t;
auto log2Ceil(const std::size_t x) -> std::uint8_t;
auto pow2(const std::uint8_t power) -> std::size_t;
void invoke(const O1HeapHook hook);
}

constexpr auto FragmentSizeMin = O1HEAP_ALIGNMENT * 2U;
constexpr auto FragmentSizeMax = (std::numeric_limits<std::size_t>::max() >> 1U) + 1U;

static_assert((FragmentSizeMin & (FragmentSizeMin - 1U)) == 0U);
static_assert((FragmentSizeMax & (FragmentSizeMax - 1U)) == 0U);

struct Fragment;

struct FragmentHeader final
{
    Fragment*   next = nullptr;
    Fragment*   prev = nullptr;
    std::size_t size = 0U;
    bool        used = false;
};

struct Fragment final
{
    FragmentHeader header;

    Fragment* next_free = nullptr;
    Fragment* prev_free = nullptr;

    [[nodiscard]] static auto constructFromAllocatedMemory(const void* const memory) -> const Fragment&
    {
        if ((memory == nullptr) || (reinterpret_cast<std::size_t>(memory) <= O1HEAP_ALIGNMENT) ||
            (reinterpret_cast<std::size_t>(memory) % O1HEAP_ALIGNMENT) != 0U)
        {
            throw std::invalid_argument("Invalid pointer");
        }
        return *reinterpret_cast<const Fragment*>(
            reinterpret_cast<const void*>(reinterpret_cast<const std::byte*>(memory) - O1HEAP_ALIGNMENT));
    }

    [[nodiscard]] auto getBinIndex() const -> std::uint8_t
    {
        const bool aligned  = (header.size % FragmentSizeMin) == 0U;
        const bool nonempty = header.size >= FragmentSizeMin;
        if (aligned && nonempty)
        {
            return static_cast<std::uint8_t>(std::floor(std::log2(header.size / FragmentSizeMin)));
        }
        throw std::logic_error("Invalid fragment size");
    }
};

struct O1HeapInstance final
{
    std::array<Fragment*, sizeof(std::size_t) * 8U> bins{};

    std::size_t nonempty_bin_mask = 0;

    O1HeapHook critical_section_enter = nullptr;
    O1HeapHook critical_section_leave = nullptr;

    O1HeapDiagnostics diagnostics{};

    [[nodiscard]] auto allocate(const size_t amount)
    {
        validateInvariants();  // Can't use RAII because it may throw -- can't throw from destructor.
        const auto out = o1heapAllocate(reinterpret_cast<::O1HeapInstance*>(this), amount);
        validateInvariants();
        return out;
    }

    auto free(void* const pointer)
    {
        validateInvariants();
        o1heapFree(reinterpret_cast<::O1HeapInstance*>(this), pointer);
        validateInvariants();
    }

    [[nodiscard]] auto getDiagnostics() const
    {
        validateInvariants();
        const auto out = o1heapGetDiagnostics(reinterpret_cast<const ::O1HeapInstance*>(this));
        validateInvariants();
        REQUIRE(std::memcmp(&diagnostics, &out, sizeof(diagnostics)) == 0);
        return out;
    }

    [[nodiscard]] auto getFirstFragment() const
    {
        const std::uint8_t* ptr = reinterpret_cast<const std::uint8_t*>(this) + sizeof(*this);
        while ((reinterpret_cast<std::size_t>(ptr) % O1HEAP_ALIGNMENT) != 0)
        {
            ptr++;
        }
        const auto frag = reinterpret_cast<const Fragment*>(reinterpret_cast<const void*>(ptr));
        // Apply heuristics to make sure the fragment is found correctly.
        REQUIRE(frag->header.size >= FragmentSizeMin);
        REQUIRE(frag->header.size <= FragmentSizeMax);
        REQUIRE(frag->header.size <= diagnostics.capacity);
        REQUIRE((frag->header.size % FragmentSizeMin) == 0U);
        REQUIRE(((frag->header.next == nullptr) || (frag->header.next->header.prev == frag)));
        REQUIRE(frag->header.prev == nullptr);  // The first fragment has no prev!
        REQUIRE(frag->prev_free == nullptr);    // The first fragment has no prev!
        return frag;
    }

    void validateInvariants() const
    {
        // Validate the core structure.
        REQUIRE(diagnostics.capacity >= FragmentSizeMin);
        REQUIRE(diagnostics.capacity <= FragmentSizeMax);
        REQUIRE((diagnostics.capacity % FragmentSizeMin) == 0U);

        REQUIRE(diagnostics.allocated <= diagnostics.capacity);
        REQUIRE((diagnostics.allocated % FragmentSizeMin) == 0U);

        REQUIRE(diagnostics.peak_allocated <= diagnostics.capacity);
        REQUIRE(diagnostics.peak_allocated >= diagnostics.allocated);
        REQUIRE((diagnostics.peak_allocated % FragmentSizeMin) == 0U);

        REQUIRE(((diagnostics.peak_request_size <= diagnostics.capacity) || (diagnostics.oom_count > 0U)));

        // Traverse the list of blocks and make sure everything is OK there.
        // Check the total size, i.e., no blocks are missing.
        {
            std::size_t pending_bins = 0U;
            for (std::size_t i = 0U; i < std::size(bins); i++)
            {
                if (bins.at(i) != nullptr)
                {
                    pending_bins |= static_cast<std::size_t>(1) << i;
                }
            }
            // Ensure the bin lookup mask is in sync with the bins.
            REQUIRE(pending_bins == nonempty_bin_mask);

            std::size_t total_size      = 0U;
            std::size_t total_allocated = 0U;

            auto frag = getFirstFragment();
            do
            {
                const auto frag_address = reinterpret_cast<std::size_t>(frag);
                REQUIRE((frag_address % sizeof(void*)) == 0U);

                // Size correctness.
                REQUIRE(frag->header.size >= FragmentSizeMin);
                REQUIRE(frag->header.size <= FragmentSizeMax);
                REQUIRE(frag->header.size <= diagnostics.capacity);
                REQUIRE((frag->header.size % FragmentSizeMin) == 0U);

                // Heap fragment interlinking.
                if (frag->header.next != nullptr)
                {
                    const auto adr = reinterpret_cast<std::size_t>(frag->header.next);
                    REQUIRE((adr % sizeof(void*)) == 0U);
                    REQUIRE(frag->header.next->header.prev == frag);
                    REQUIRE(adr > frag_address);
                    REQUIRE(((adr - frag_address) % FragmentSizeMin) == 0U);
                }
                if (frag->header.prev != nullptr)
                {
                    const auto adr = reinterpret_cast<std::size_t>(frag->header.prev);
                    REQUIRE((adr % sizeof(void*)) == 0U);
                    REQUIRE(frag->header.prev->header.next == frag);
                    REQUIRE(frag_address > adr);
                    REQUIRE(((frag_address - adr) % FragmentSizeMin) == 0U);
                }

                // Segregated free list interlinking.
                if (!frag->header.used)
                {
                    if (frag->next_free != nullptr)
                    {
                        REQUIRE(frag->next_free->prev_free == frag);
                        REQUIRE(!frag->next_free->header.used);
                    }
                    if (frag->prev_free != nullptr)
                    {
                        REQUIRE(frag->prev_free->next_free == frag);
                        REQUIRE(!frag->prev_free->header.used);
                    }
                }

                // Update and check the totals early.
                total_size += frag->header.size;
                REQUIRE(total_size <= FragmentSizeMax);
                REQUIRE(total_size <= diagnostics.capacity);
                REQUIRE((total_size % FragmentSizeMin) == 0U);
                if (frag->header.used)
                {
                    total_allocated += frag->header.size;
                    REQUIRE(total_allocated <= total_size);
                    REQUIRE((total_allocated % FragmentSizeMin) == 0U);
                    // Ensure no bin links to a used fragment.
                    REQUIRE(bins.at(frag->getBinIndex()) != frag);
                }
                else
                {
                    const std::size_t mask = static_cast<std::size_t>(1) << frag->getBinIndex();
                    REQUIRE((nonempty_bin_mask & mask) != 0U);
                    if (bins.at(frag->getBinIndex()) == frag)
                    {
                        REQUIRE((pending_bins & mask) != 0U);
                        pending_bins &= ~mask;
                    }
                }

                frag = frag->header.next;
            } while (frag != nullptr);

            // Ensure there were no hanging bin pointers.
            REQUIRE(pending_bins == 0);

            // Validate the totals.
            REQUIRE(total_size == diagnostics.capacity);
            REQUIRE(total_allocated == diagnostics.allocated);
        }

        // Check the segregated list bins.
        {
            std::size_t total_free = 0U;

            for (std::size_t i = 0U; i < std::size(bins); i++)
            {
                const std::size_t mask = static_cast<std::size_t>(1) << i;
                const std::size_t min  = internal::FragmentSizeMin << i;
                const std::size_t max  = (internal::FragmentSizeMin << i) * 2U - 1U;

                auto frag = bins.at(i);
                if (frag != nullptr)
                {
                    REQUIRE((nonempty_bin_mask & mask) != 0U);
                    REQUIRE(!frag->header.used);
                    REQUIRE(frag->prev_free == nullptr);  // The first fragment in the segregated list has no prev.
                    do
                    {
                        REQUIRE(frag->header.size >= min);
                        REQUIRE(frag->header.size <= max);

                        total_free += frag->header.size;

                        if (frag->next_free != nullptr)
                        {
                            REQUIRE(frag->next_free->prev_free == frag);
                            REQUIRE(!frag->next_free->header.used);
                        }
                        if (frag->prev_free != nullptr)
                        {
                            REQUIRE(frag->prev_free->next_free == frag);
                            REQUIRE(!frag->prev_free->header.used);
                        }

                        frag = frag->next_free;
                    } while (frag != nullptr);
                }
                else
                {
                    REQUIRE((nonempty_bin_mask & mask) == 0U);
                }
            }

            REQUIRE((diagnostics.capacity - diagnostics.allocated) == total_free);
        }
    }

    O1HeapInstance()                       = delete;
    O1HeapInstance(const O1HeapInstance&)  = delete;
    O1HeapInstance(const O1HeapInstance&&) = delete;
    ~O1HeapInstance()                      = delete;
    auto operator=(const O1HeapInstance&) -> O1HeapInstance& = delete;
    auto operator=(const O1HeapInstance &&) -> O1HeapInstance& = delete;
};

}  // namespace internal

#endif  // O1HEAP_TESTS_INTERNAL_HPP_INCLUDED
