// https://raw.githubusercontent.com/zmb3/hexdump/master/Hexdump.hpp

#ifndef HEXDUMP_HPP
#define HEXDUMP_HPP

#include <bitset>
#include <cctype>
#include <iomanip>
#include <ostream>

namespace util
{
template <unsigned RowSize,
          bool ShowAscii,
          bool ShowLn,
          bool InHex,
          bool Newline>
struct Customdump
{
    Customdump(volatile const void *data, unsigned length)
        : mData(static_cast<volatile const unsigned char *>(data)),
          mLength(length)
    {
    }
    const volatile unsigned char *mData;
    const unsigned mLength;
};

template <unsigned RowSize,
          bool ShowAscii,
          bool ShowLn,
          bool InHex,
          bool Newline>
std::ostream &operator<<(
    std::ostream &out,
    const Customdump<RowSize, ShowAscii, ShowLn, InHex, Newline> &dump)
{
    auto flags = out.flags();
    out.fill('0');

    for (size_t i = 0; i < dump.mLength; i += RowSize)
    {
        if (ShowLn)
        {
            out << "0x" << std::setw(6) << std::hex << i << ": ";
        }
        for (size_t j = 0; j < RowSize; ++j)
        {
            if (i + j < dump.mLength)
            {
                if (InHex)
                {
                    out << std::hex << std::setw(2)
                        << static_cast<int>(dump.mData[i + j]) << " ";
                }
                else
                {
                    out << std::bitset<8>(dump.mData[i + j]) << " ";
                }
            }
            else
            {
                out << "   ";
            }
        }

        out << " ";
        if (ShowAscii)
        {
            for (size_t j = 0; j < RowSize; ++j)
            {
                if (i + j < dump.mLength)
                {
                    if (std::isprint(dump.mData[i + j]))
                    {
                        out << static_cast<char>(dump.mData[i + j]);
                    }
                    else
                    {
                        out << ".";
                    }
                }
            }
        }
        if (Newline)
        {
            out << std::endl;
        }
    }
    out.flags(flags);
    return out;
}
using Hexdump = Customdump<16, true, true, true, true>;
using Bindump = Customdump<8, true, true, false, true>;
using InlinedHexdump = Customdump<16, false, false, true, false>;
using InlinedBindump = Customdump<8, false, false, false, false>;

template <unsigned RowSize, bool InHex>
struct PreBitsCustom
{
    PreBitsCustom(void *data, size_t size)
        : data_((unsigned char *) data), size_(size)
    {
    }
    unsigned char *data_;
    size_t size_;
};

template <unsigned RowSize, bool InHex>
inline std::ostream &operator<<(std::ostream &os,
                                const PreBitsCustom<RowSize, InHex> &b)
{
    auto flags = os.flags();
    size_t idx = 0;
    for (ssize_t i = b.size_ - 1; i >= 0; --i)
    {
        if (InHex)
        {
            os << std::hex << std::setw(2) << (int) b.data_[i] << " ";
        }
        else
        {
            os << std::bitset<8>(b.data_[i]) << " ";
        }
        idx++;
        bool end_line = idx % RowSize == 0;
        if (end_line)
        {
            os << std::endl;
        }
    }
    os.flags(flags);
    return os;
}

// Pre[Hex|Bin]Bits show the bit in the human-readable order
// i.e., lower address on the right and upper address on the left.
// Useful to debug bit operations.
// using PreHexBits = PreBitsCustom<true>;
// using PreBinBits = PreBitsCustom<false>;

}  // namespace util

#endif  // HEXDUMP_HPP