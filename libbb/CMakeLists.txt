cmake_minimum_required(VERSION 2.6)

PROJECT(libbb C)

ADD_LIBRARY(bb STATIC
	concat_path_file.c copy_file.c copy_file_chunk.c gzip.c
	last_char_is.c make_directory.c mode_string.c
	safe_strncpy.c time_string.c unarchive.c wfopen.c xfuncs.c
	xreadlink.c
)
