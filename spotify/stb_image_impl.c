/* stb_image (public domain / MIT, pinned with stb_vorbis) for album cover
 * JPEGs and theme wallpapers (JPEG or PNG). Separate from the ME-checked stb_vorbis library: this runs on the
 * SC only and uses malloc. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
/* The zlib decoder also inflates gzip-compressed Spotify Connect pushes. */
#define STBI_SUPPORT_ZLIB
#include "stb_image.h"
