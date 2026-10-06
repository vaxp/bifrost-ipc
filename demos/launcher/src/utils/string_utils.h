#pragma once

#include <stdbool.h>

/**
 * Case-insensitive UTF-8 substring search.
 * Returns true if @needle is found in @haystack.
 */
bool str_contains_icase (const char *haystack, const char *needle);

/**
 * Normalize a string for search: lowercase, collapse whitespace.
 * Returns newly allocated string — caller must g_free().
 */
char *str_normalize (const char *input);
