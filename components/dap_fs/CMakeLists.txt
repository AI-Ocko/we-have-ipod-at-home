# Code that walks the file system but needs no hardware: it uses only the C
# library and POSIX dirent.h / sys/stat.h, so the same sources also build
# with the host compiler under test/host.
idf_component_register(SRCS "libscan.c"
                       INCLUDE_DIRS "."
                       REQUIRES dap_core)
