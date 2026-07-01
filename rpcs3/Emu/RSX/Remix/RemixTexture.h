#pragma once

#include "util/types.hpp"

#include <string>

namespace rsx
{
	class fragment_texture;
}

namespace rsx::remix
{
	struct texture_capture_result
	{
		// Stable identity hash for the source texture. Used as the Remix material hash so
		// that every distinct PS3 texture maps to a distinct, replaceable Remix surface.
		u64 hash = 0;

		// Absolute utf8 path to an on-disk RGBA image decoded from the texture, or empty
		// when the format is unsupported / capture is disabled / decoding failed.
		std::string albedo_path;

		// True when at least a stable identity hash was produced.
		bool valid = false;
	};

	// Produces a stable identity hash for the given fragment texture and, when the format
	// is supported and texture capture is enabled, decodes+caches its base mip level as an
	// albedo image on disk (reusing RPCS3's own deswizzle / byteswap / DXT decoders).
	//
	// Results are cached by a cheap per-texture key so each unique texture is only hashed,
	// decoded and written once. Safe to call from the RSX render thread; on any error the
	// returned result simply has valid == false and the caller falls back to flat shading.
	texture_capture_result capture_fragment_texture(const rsx::fragment_texture& tex);

	// Clears the in-memory capture cache (call when the Remix runtime shuts down).
	void reset_texture_cache();
}
