#include "stdafx.h"
#include "RemixTexture.h"

#include "Emu/Memory/vm.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/RSXTexture.h"
#include "Emu/RSX/gcm_enums.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/system_config.h"
#include "Utilities/File.h"

#include "png.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rsx::remix
{
	namespace
	{
		// Safety ceilings. These bound the CPU work and disk usage of texture capture so a
		// pathological scene can never stall the render thread or fill the disk. Raised well
		// above the original 4096px / 8 MiB so 2048² 32-bit and 4096² compressed atlases (with
		// mip chains) are no longer skipped wholesale.
		constexpr u32 max_capture_dim = 8192;
		constexpr usz max_capture_bytes = 32u * 1024 * 1024;
		constexpr usz max_session_captures = 32768;

		std::mutex g_cache_mutex;
		std::unordered_map<u64, texture_capture_result> g_cache;
		usz g_dumped_files = 0;

		u64 mix_u64(u64 hash, u64 value)
		{
			hash ^= value;
			hash *= 1099511628211ull;
			return hash;
		}

		u64 hash_bytes(const void* data, usz size)
		{
			const auto* p = static_cast<const u8*>(data);
			u64 hash = 1469598103934665603ull;

			usz i = 0;
			for (; i + 8 <= size; i += 8)
			{
				u64 value = 0;
				std::memcpy(&value, p + i, sizeof(value));
				hash = mix_u64(hash, value);
			}

			for (; i < size; i++)
			{
				hash ^= p[i];
				hash *= 1099511628211ull;
			}

			return hash;
		}

		std::string to_hex16(u64 value)
		{
			static constexpr char digits[] = "0123456789abcdef";
			std::string out(16, '0');
			for (int i = 15; i >= 0; i--)
			{
				out[i] = digits[value & 0xf];
				value >>= 4;
			}
			return out;
		}

		// How the tightly-packed host output produced by upload_texture_subresource (with every
		// hardware fast-path disabled) must be reinterpreted to obtain an RGBA8 albedo pixel.
		enum class decode_kind : u8
		{
			bgra8,    // u32, host bytes [B, G, R, A]
			rgb565,   // u16 R5G6B5
			argb1555, // u16 A1R5G5B5
			rgba5551, // u16 R5G5B5A1
			argb4444, // u16 A4R4G4B4
			rgb655,   // u16 R6G5B5
			gr88,     // u16 G8B8 (two 8-bit channels; shown as luminance)
			r8,       // u8  B8   (single 8-bit channel; shown as luminance)
		};

		struct albedo_format_desc
		{
			bool supported = false;
			u8 host_word_bytes = 0;   // size of one decoded host element
			u8 required_alignment = 0; // source pointer alignment required by the decoder
			decode_kind kind = decode_kind::bgra8;
			bool force_opaque = false; // formats without a real alpha channel
		};

		// Describes every GCM colour format that upload_texture_subresource can lower to a
		// displayable image on the host. Depth / HILO / raw float formats are intentionally
		// excluded: they carry non-colour data and would look like garbage as an albedo hint.
		albedo_format_desc describe_albedo_format(u32 base_format)
		{
			switch (base_format)
			{
			// Decode straight to BGRA8 (u32) host words.
			case CELL_GCM_TEXTURE_A8R8G8B8:            return { true, 4, 4, decode_kind::bgra8, false };
			case CELL_GCM_TEXTURE_D8R8G8B8:            return { true, 4, 4, decode_kind::bgra8, true };
			case CELL_GCM_TEXTURE_COMPRESSED_DXT1:     return { true, 4, 8, decode_kind::bgra8, false };
			case CELL_GCM_TEXTURE_COMPRESSED_DXT23:    return { true, 4, 16, decode_kind::bgra8, false };
			case CELL_GCM_TEXTURE_COMPRESSED_DXT45:    return { true, 4, 16, decode_kind::bgra8, false };
			case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:return { true, 4, 4, decode_kind::bgra8, true };
			case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:return { true, 4, 4, decode_kind::bgra8, true };

			// 16-bit packed colour formats. On non-Apple hosts these stay raw 16-bit, so we
			// unpack the bitfields to RGBA8 ourselves.
			case CELL_GCM_TEXTURE_R5G6B5:              return { true, 2, 2, decode_kind::rgb565, true };
			case CELL_GCM_TEXTURE_A1R5G5B5:            return { true, 2, 2, decode_kind::argb1555, false };
			case CELL_GCM_TEXTURE_D1R5G5B5:            return { true, 2, 2, decode_kind::argb1555, true };
			case CELL_GCM_TEXTURE_R5G5B5A1:            return { true, 2, 2, decode_kind::rgba5551, false };
			case CELL_GCM_TEXTURE_A4R4G4B4:            return { true, 2, 2, decode_kind::argb4444, false };
			case CELL_GCM_TEXTURE_R6G5B5:              return { true, 2, 2, decode_kind::rgb655, true };
			case CELL_GCM_TEXTURE_G8B8:                return { true, 2, 2, decode_kind::gr88, true };

			// Single-channel luminance.
			case CELL_GCM_TEXTURE_B8:                  return { true, 1, 1, decode_kind::r8, true };

			default:
				return {};
			}
		}

		// Expand five/six-bit channels to eight bits with proper rounding (bit replication).
		constexpr u8 expand5(u32 v) { return static_cast<u8>((v << 3) | (v >> 2)); }
		constexpr u8 expand6(u32 v) { return static_cast<u8>((v << 2) | (v >> 4)); }
		constexpr u8 expand4(u32 v) { return static_cast<u8>((v << 4) | v); }

		// Convert one decoded host element at `src` into an RGBA8 quad. `src` points at
		// host_word_bytes bytes produced by upload_texture_subresource (native endianness).
		void expand_pixel(const u8* src, decode_kind kind, bool force_opaque, u8* out_rgba)
		{
			u8 r = 0, g = 0, b = 0, a = 255;

			switch (kind)
			{
			case decode_kind::bgra8:
			{
				b = src[0];
				g = src[1];
				r = src[2];
				a = force_opaque ? 255 : src[3];
				break;
			}
			case decode_kind::rgb565:
			{
				u16 v = 0; std::memcpy(&v, src, sizeof(v));
				r = expand5((v >> 11) & 0x1f);
				g = expand6((v >> 5) & 0x3f);
				b = expand5(v & 0x1f);
				break;
			}
			case decode_kind::argb1555:
			{
				u16 v = 0; std::memcpy(&v, src, sizeof(v));
				r = expand5((v >> 10) & 0x1f);
				g = expand5((v >> 5) & 0x1f);
				b = expand5(v & 0x1f);
				a = force_opaque ? 255 : (((v >> 15) & 0x1) ? 255 : 0);
				break;
			}
			case decode_kind::rgba5551:
			{
				u16 v = 0; std::memcpy(&v, src, sizeof(v));
				r = expand5((v >> 11) & 0x1f);
				g = expand5((v >> 6) & 0x1f);
				b = expand5((v >> 1) & 0x1f);
				a = force_opaque ? 255 : ((v & 0x1) ? 255 : 0);
				break;
			}
			case decode_kind::argb4444:
			{
				u16 v = 0; std::memcpy(&v, src, sizeof(v));
				r = expand4((v >> 8) & 0xf);
				g = expand4((v >> 4) & 0xf);
				b = expand4(v & 0xf);
				a = force_opaque ? 255 : expand4((v >> 12) & 0xf);
				break;
			}
			case decode_kind::rgb655:
			{
				u16 v = 0; std::memcpy(&v, src, sizeof(v));
				r = expand6((v >> 10) & 0x3f);
				g = expand5((v >> 5) & 0x1f);
				b = expand5(v & 0x1f);
				break;
			}
			case decode_kind::gr88:
			{
				// Native u16 after byteswap is (G << 8) | B. Show as luminance from G so
				// two-channel data (e.g. baked normal/gloss) still reads as a sensible albedo.
				u16 v = 0; std::memcpy(&v, src, sizeof(v));
				r = g = b = static_cast<u8>(v >> 8);
				break;
			}
			case decode_kind::r8:
			{
				r = g = b = src[0];
				break;
			}
			}

			out_rgba[0] = r;
			out_rgba[1] = g;
			out_rgba[2] = b;
			out_rgba[3] = a;
		}

		// Encode tightly-packed RGBA8 to an in-memory PNG buffer using libpng (mirrors the
		// screenshot encoder in gs_frame.cpp). Returns false on failure.
		bool encode_png_rgba(const std::vector<u8>& rgba, u32 width, u32 height, std::vector<u8>& out)
		{
			if (!width || !height || rgba.size() < static_cast<usz>(width) * height * 4)
			{
				return false;
			}

			png_structp write_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
			if (!write_ptr)
			{
				return false;
			}

			png_infop info_ptr = png_create_info_struct(write_ptr);
			if (!info_ptr)
			{
				png_destroy_write_struct(&write_ptr, nullptr);
				return false;
			}

			out.clear();
			out.reserve(static_cast<usz>(width) * height);

			png_set_write_fn(write_ptr, &out,
				[](png_structp png_ptr, png_bytep data, png_size_t length)
				{
					auto* buffer = static_cast<std::vector<u8>*>(png_get_io_ptr(png_ptr));
					buffer->insert(buffer->end(), data, data + length);
				},
				nullptr);

			png_set_IHDR(write_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGBA,
				PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

			std::vector<png_bytep> rows(height);
			for (u32 y = 0; y < height; y++)
			{
				rows[y] = const_cast<png_bytep>(rgba.data() + static_cast<usz>(y) * width * 4);
			}

			png_set_rows(write_ptr, info_ptr, rows.data());
			png_write_png(write_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, nullptr);

			png_free_data(write_ptr, info_ptr, PNG_FREE_ALL, -1);
			png_destroy_write_struct(&write_ptr, &info_ptr);
			return !out.empty();
		}

		// Decode the base mip of `tex` (already known to be a supported 2D format) into an
		// RGBA8 PNG on disk. Returns the file path, or an empty string on failure.
		std::string dump_albedo_png(const rsx::fragment_texture& tex, u32 base_format, const albedo_format_desc& desc, u64 content_hash)
		{
			{
				std::lock_guard lock(g_cache_mutex);
				if (g_dumped_files >= max_session_captures)
				{
					return {};
				}
			}

			const std::string dir = fs::get_cache_dir() + "remix_textures/";
			const std::string path = dir + "remix_" + to_hex16(content_hash) + ".png";

			// A capture with this exact content already exists (e.g. from a previous run).
			if (fs::is_file(path))
			{
				return path;
			}

			// Reuse RPCS3's own decoders: deswizzle + byteswap + DXT decompression all happen
			// inside upload_texture_subresource once we disable every hardware fast-path.
			const std::vector<rsx::subresource_layout> layouts = rsx::get_subresources_layout(tex);
			if (layouts.empty())
			{
				return {};
			}

			const rsx::subresource_layout& mip0 = layouts.front();
			if (mip0.border != 0 || !mip0.width_in_texel || !mip0.height_in_texel)
			{
				return {};
			}

			const u32 width = mip0.width_in_texel;
			const u32 height = mip0.height_in_texel;

			const u8 block_bytes = rsx::get_format_block_size_in_bytes(static_cast<int>(base_format));
			const u32 src_row_pitch = mip0.pitch_in_block * block_bytes;
			// The decoded host output uses host_word_bytes per texel (4 for BGRA8/DXT, 2 for the
			// 16-bit packed formats, 1 for B8). Using it here mirrors the exact pitch arithmetic
			// upload_texture_subresource performs internally, so rows come out tightly packed.
			const u32 host_packed_pitch = desc.host_word_bytes * width;
			const u32 row_pitch = std::max(src_row_pitch, host_packed_pitch);
			const usz decoded_size = static_cast<usz>(row_pitch) * height;

			// A little slack; upload_texture_subresource may realign by a few bytes.
			std::vector<u8> decoded(decoded_size + 16, 0);

			rsx::texture_uploader_capabilities caps{};
			caps.supports_byteswap = false;     // force CPU byteswap -> host-endian words
			caps.supports_vtc_decoding = false;
			caps.supports_hw_deswizzle = false; // force CPU deswizzle
			caps.supports_zero_copy = false;    // write directly into our buffer
			caps.supports_dxt = false;          // force CPU DXT -> BGRA8 decode
			caps.alignment = row_pitch;         // yields a tightly-packed destination

			const bool is_swizzled = !(tex.format() & CELL_GCM_TEXTURE_LN);

			rsx::io_buffer dst(decoded.data(), decoded.size());
			rsx::upload_texture_subresource(dst, mip0, static_cast<int>(base_format), is_swizzled, caps);

			// Expand the decoded host words -> RGBA8, dropping any row padding.
			std::vector<u8> rgba(static_cast<usz>(width) * height * 4);
			for (u32 y = 0; y < height; y++)
			{
				const u8* src_row = decoded.data() + static_cast<usz>(y) * row_pitch;
				u8* dst_row = rgba.data() + static_cast<usz>(y) * width * 4;
				for (u32 x = 0; x < width; x++)
				{
					expand_pixel(src_row + static_cast<usz>(x) * desc.host_word_bytes, desc.kind, desc.force_opaque, dst_row + x * 4);
				}
			}

			std::vector<u8> png;
			if (!encode_png_rgba(rgba, width, height, png))
			{
				return {};
			}

			fs::create_path(dir);

			fs::file file(path, fs::rewrite);
			if (!file)
			{
				return {};
			}

			file.write(png.data(), png.size());
			file.close();

			{
				std::lock_guard lock(g_cache_mutex);
				g_dumped_files++;
			}

			return path;
		}
	}

	texture_capture_result capture_fragment_texture(const rsx::fragment_texture& tex)
	{
		texture_capture_result result{};

		try
		{
			if (!tex.width() || !tex.height())
			{
				return result;
			}

			const u32 raw_format = tex.format();
			const u32 base_format = raw_format & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
			const u16 width = tex.width();
			const u16 height = tex.height();
			const u16 mipmaps = tex.get_exact_mipmap_count();
			const u32 pitch = tex.pitch();

			// Cheap, stable per-texture key used for the capture cache.
			u64 key = 1469598103934665603ull;
			key = mix_u64(key, tex.offset());
			key = mix_u64(key, tex.location());
			key = mix_u64(key, base_format);
			key = mix_u64(key, (static_cast<u64>(width) << 16) | height);
			key = mix_u64(key, (static_cast<u64>(mipmaps) << 24) | pitch);

			{
				std::lock_guard lock(g_cache_mutex);
				if (const auto it = g_cache.find(key); it != g_cache.end())
				{
					return it->second;
				}
			}

			u64 content_hash = key;
			std::string albedo_path;

			const usz tex_size = rsx::get_texture_size(tex);
			if (tex_size && tex_size <= max_capture_bytes)
			{
				const u32 addr = rsx::get_address(tex.offset(), tex.location(), static_cast<u32>(tex_size));
				if (addr)
				{
					// A content hash gives stable identity across runs (better for Remix mods).
					content_hash = hash_bytes(vm::_ptr<const std::byte>(addr), tex_size);

					// upload_texture_subresource reads the source through aligned spans (u16/u32 for
					// packed formats, u64/u128 for the DXT decoders) and RPCS3's ensure() aborts on a
					// misaligned pointer, so only attempt the pixel dump when the base address
					// satisfies the alignment that this specific format's decoder requires.
					const albedo_format_desc desc = describe_albedo_format(base_format);

					if (desc.supported &&
						g_cfg.video.rtx_remix.capture_textures.get() &&
						(addr % desc.required_alignment) == 0 &&
						tex.get_extended_texture_dimension() == rsx::texture_dimension_extended::texture_dimension_2d &&
						width <= max_capture_dim && height <= max_capture_dim)
					{
						albedo_path = dump_albedo_png(tex, base_format, desc, content_hash);
					}
				}
			}

			result.hash = content_hash;
			result.albedo_path = std::move(albedo_path);
			result.valid = true;

			{
				std::lock_guard lock(g_cache_mutex);
				g_cache.emplace(key, result);
			}

			return result;
		}
		catch (...)
		{
			// Never let texture capture take down the render thread.
			return texture_capture_result{};
		}
	}

	void reset_texture_cache()
	{
		std::lock_guard lock(g_cache_mutex);
		g_cache.clear();
		g_dumped_files = 0;
	}
}
