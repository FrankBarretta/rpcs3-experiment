#pragma once

#include "Emu/RSX/display.h"
#include "util/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rsx::remix
{
	struct mesh_vertex
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		// Object-space normal decoded from RSX ATTR2. Defaults to facing the camera so meshes
		// without a normal stream still shade sensibly under Remix's path tracer.
		float nx = 0.0f;
		float ny = 0.0f;
		float nz = -1.0f;
		float u = 0.0f;
		float v = 0.0f;
		u32 color = 0xffffffffu;
	};

	struct mesh_capture
	{
		std::vector<mesh_vertex> vertices;
		std::vector<u32> indices;
		u64 hash = 0;
		u64 material_hash = 0;
		float albedo[3] = { 1.0f, 1.0f, 1.0f };
		float opacity = 1.0f;
		// Filesystem path (utf8) of the albedo texture captured from the bound RSX
		// fragment texture. Empty when no usable texture was captured for this draw.
		std::string albedo_texture_path;
		// Row-major world->clip view-projection recovered from the game's vertex-program
		// constants for this draw (clip = M * (x, y, z, 1)). Only valid when has_view_proj
		// is set; the bridge uses it to reconstruct the game's real camera.
		float view_proj[16] = {};
		bool has_view_proj = false;
	};

	class bridge final
	{
	public:
		bridge();
		~bridge();

		bridge(const bridge&) = delete;
		bridge& operator=(const bridge&) = delete;
		bridge(bridge&&) = delete;
		bridge& operator=(bridge&&) = delete;

		bool is_enabled() const;
		bool is_running() const;

		void initialize(display_handle_t window_handle);
		void begin_frame(u32 width, u32 height);
		void note_rsx_draw(u32 vertex_count, bool indexed, u32 instance_count);
		void submit_mesh(const mesh_capture& mesh);
		void present(display_handle_t window_handle, u32 width, u32 height);
		void shutdown();

	private:
		struct impl;
		std::unique_ptr<impl> m_impl;
	};
}
