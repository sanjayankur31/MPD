/*
 * Copyright 2003-2016 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_UPNP_UTIL_HXX
#define MPD_UPNP_UTIL_HXX

#include "Compiler.h"

#include <string>
#include <list>

void
trimstring(std::string &s, const char *ws = " \t\n");

std::string
path_getfather(const std::string &s);

gcc_pure
std::list<std::string>
stringToTokens(const std::string &str,
	       const char *delims = "/", bool skipinit = true);

template <class T>
bool
csvToStrings(const char *s, T &tokens);

#endif /* _UPNPP_H_X_INCLUDED_ */
