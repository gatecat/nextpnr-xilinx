/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019-2020  David Shah <dave@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// Some data structures to support the RippleFPGA placer

#ifndef PLACER_RIPPLE_UTIL_H
#define PLACER_RIPPLE_UTIL_H

#include <vector>
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace Ripple {

// "Slotted" indexed object store
template <typename T> class indexed_store
{
  private:
    // This should move to using std::optional at some point
    class slot
    {
      private:
        bool active;
        alignas(T) unsigned char storage[sizeof(T)];
        int next_free;
        inline T &obj() { return reinterpret_cast<T &>(storage); }
        inline const T &obj() const { return reinterpret_cast<const T &>(storage); }
        friend class indexed_store<T>;

      public:
        slot() : active(false), next_free(std::numeric_limits<int>::max()){};
        slot(const slot &other) : active(other.active), next_free(other.next_free)
        {
            if (active)
                ::new (static_cast<void *>(&storage)) T(other.obj());
        }
        slot(slot &&other) : active(other.active), next_free(other.next_free)
        {
            if (active)
                ::new (static_cast<void *>(&storage)) T(std::move(other.obj()));
        };

        template <class... Args> void create(Args &&... args)
        {
            NPNR_ASSERT(!active);
            active = true;
            ::new (static_cast<void *>(&storage)) T(std::forward<Args &&>(args)...);
        }
        bool empty() const { return !active; }
        T &get()
        {
            NPNR_ASSERT(active);
            return reinterpret_cast<T &>(storage);
        }
        const T &get() const
        {
            NPNR_ASSERT(active);
            return reinterpret_cast<const T &>(storage);
        }
        void free(int first_free)
        {
            NPNR_ASSERT(active);
            obj().~T();
            active = false;
            next_free = first_free;
        }
        ~slot()
        {
            if (active)
                obj().~T();
        }
    };
    std::vector<slot> slots;
    int first_free = 0;

  public:
    // Create a new entry and return its index
    template <class... Args> int add(Args &&... args)
    {
        if (first_free == GetSize(slots)) {
            slots.emplace_back();
            slots.back().create(std::forward<Args &&>(args)...);
            ++first_free;
            return GetSize(slots) - 1;
        } else {
            int idx = first_free;
            auto &slot = slots.at(idx);
            first_free = slot.next_free;
            slot.create(std::forward<Args &&>(args)...);
            return idx;
        }
    }

    // Remove an entry at an index
    void remove(int idx)
    {
        slots.at(idx).free(first_free);
        first_free = idx;
    }

    // Reserve a certain amount of space
    void reserve(int size) { slots.reserve(size); }

    // Check if an index exists
    int count(int idx)
    {
        if (idx < 0 || idx >= GetSize(slots))
            return 0;
        return slots.at(idx).empty() ? 0 : 1;
    }

    // Get an item by index
    T &at(int idx) { return slots.at(idx).get(); }
    const T &at(int idx) const { return slots.at(idx).get(); }
    T &operator[](int idx) { return slots.at(idx).get(); }
    const T &operator[](int idx) const { return slots.at(idx).get(); }

    // Total size of the container
    int size() const { return GetSize(slots); }

    // Iterate over items
    class iterator
    {
      private:
        indexed_store *base;
        int index = 0;

      public:
        iterator(indexed_store *base, int index) : base(base), index(index){};
        inline bool operator!=(const iterator &other) const { return other.index != index; }
        inline bool operator==(const iterator &other) const { return other.index == index; }
        inline iterator operator++()
        {
            // skip over unused slots
            do {
                index++;
            } while (index < GetSize(base->slots) && !base->slots.at(index).active);
            return *this;
        }
        inline iterator operator++(int)
        {
            iterator prior(*this);
            do {
                index++;
            } while (index < GetSize(base->slots) && !base->slots.at(index).active);
            return prior;
        }
        T &operator*() { return base->at(index); }
    };
    iterator begin() { return iterator{this, 0}; }
    iterator end() { return iterator{this, GetSize(slots)}; }
};

// A simple internal representation for a sparse system of equations Ax = rhs
// This is designed to decouple the functions that build the matrix to the engine that
// solves it, and the representation that requires
struct EquationSystem
{

    EquationSystem(size_t rows, size_t cols)
    {
        A.resize(cols);
        rhs.resize(rows);
    }

    // Simple sparse format, easy to convert to CCS for solver
    std::vector<std::vector<std::pair<int, double>>> A; // col -> (row, x[row, col]) sorted by row
    std::vector<double> rhs;                            // RHS vector
    void reset()
    {
        for (auto &col : A)
            col.clear();
        std::fill(rhs.begin(), rhs.end(), 0.0);
    }

    void add_coeff(int row, int col, double val)
    {
        auto &Ac = A.at(col);
        // Binary search
        int b = 0, e = int(Ac.size()) - 1;
        while (b <= e) {
            int i = (b + e) / 2;
            if (Ac.at(i).first == row) {
                Ac.at(i).second += val;
                return;
            }
            if (Ac.at(i).first > row)
                e = i - 1;
            else
                b = i + 1;
        }
        Ac.insert(Ac.begin() + b, std::make_pair(row, val));
    }

    void add_rhs(int row, double val) { rhs[row] += val; }

    void solve(std::vector<double> &x, float tolerance);
};

template <typename T> class array2d
{
  public:
    array2d() : m_width(0), m_height(0), m_size(0), data(nullptr){};
    array2d(int width, int height) : m_width(width), m_height(height), m_size(width * height)
    {
        data = new T[m_width * m_height]();
    }
    array2d(int width, int height, const T &init) : m_width(width), m_height(height), m_size(width * height)
    {
        data = new T[m_width * m_height];
        std::fill(data, data + (m_width * m_height), init);
    }
    void reset(int new_width, int new_height, const T &init = {})
    {
        if ((new_width * new_height) > m_size) {
            delete[] data;
            m_size = new_width * new_height;
            data = new T[m_size];
        }
        m_width = new_width;
        m_height = new_height;
        std::fill(data, data + (m_width * m_height), init);
    }

    int width() const { return m_width; }
    int height() const { return m_height; }
    T &at(int x, int y)
    {
        NPNR_ASSERT(x >= 0 && x < m_width);
        NPNR_ASSERT(y >= 0 && y < m_height);
        return data[y * m_width + x];
    }
    T &at(const Loc &l) { return at(l.x, l.y); }
    const T &at(int x, int y) const
    {
        NPNR_ASSERT(x >= 0 && x < m_width);
        NPNR_ASSERT(y >= 0 && y < m_height);
        return data[y * m_width + x];
    }
    const T &at(const Loc &l) const { return at(l.x, l.y); }
    ~array2d() { delete[] data; }
    struct entry
    {
        entry(int x, int y, T &value) : x(x), y(y), value(value){};
        int x, y;
        T &value;
    };
    struct iterator
    {
      public:
        entry operator*() { return {x, y, base->at(x, y)}; }
        inline iterator operator++()
        {
            ++x;
            if (x >= base->width()) {
                x = 0;
                ++y;
            }
            return *this;
        }
        inline iterator operator++(int)
        {
            iterator prior(x, y, base);
            ++x;
            if (x >= base->width()) {
                x = 0;
                ++y;
            }
            return prior;
        }
        inline bool operator!=(const iterator &other) const { return other.x != x || other.y != y; }
        inline bool operator==(const iterator &other) const { return other.x == x && other.y == y; }

      private:
        iterator(int x, int y, array2d<T> &base) : x(x), y(y), base(&base){};
        int x, y;
        array2d<T> *base;
        friend class array2d;
    };
    iterator begin() { return {0, 0, *this}; }
    iterator end() { return {0, m_height, *this}; }

  private:
    int m_width, m_height, m_size;
    T *data;
};

} // namespace Ripple

NEXTPNR_NAMESPACE_END

#endif
