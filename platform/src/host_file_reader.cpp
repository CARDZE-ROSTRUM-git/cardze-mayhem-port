/*
 * Copyright (C) 2023 Kyle Reed
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * Host replacement for firmware/application/file_reader.cpp.
 *
 * The upstream split_string() builds the trailing field with &str[str.length()]
 * when the input ends with the delimiter: after the last separator `start`
 * equals str.length(), and the guard `if (start <= str.length())` lets that
 * through, so it forms a view one past the end of the string_view. On the device
 * (newlib's std::string_view) operator[] does not range-check, so this silently
 * produced an empty trailing field; on a modern host libstdc++ with assertions
 * enabled the index check aborts the whole process. Every firmware modal message
 * terminates with '\n' ("Can't read external apps\n...\n"), so painting such a
 * modal — touching the ExtAppErr tile, or several paths out of the File
 * Manager — used to crash the desktop build.
 *
 * The behaviour is kept identical, including the empty trailing column the
 * upstream tests require (see test_file_reader.cpp: "hello,world," yields three
 * columns); only the out-of-bounds construction is replaced by an equivalent
 * substring, which yields the same empty view without indexing past the end.
 *
 * PortaPack desktop port. GPLv3-or-later derived work.
 */

#include "file_reader.hpp"
#include <string_view>
#include <vector>

/* Splits the string on the specified char and returns
 * a vector of string_views. NB: the lifetime of the
 * string to split must be maintained while the views
 * are used or they will dangle. */
std::vector<std::string_view> split_string(std::string_view str, char c) {
    std::vector<std::string_view> cols;
    size_t start = 0;

    while (start < str.length()) {
        auto it = str.find(c, start);

        if (it == str.npos)
            break;

        cols.emplace_back(&str[start], it - start);
        start = it + 1;
    }

    /* Append the final field. This includes the empty trailing column when the
     * string ends with the delimiter (upstream behaviour the tests pin down),
     * while an empty input still yields no columns. substr() expresses the
     * trailing field without indexing past the end of the view. */
    if (start <= str.length() && !str.empty())
        cols.emplace_back(str.substr(start));

    return cols;
}
