#pragma once

#include "Emu/RSX/display.h"
#include "util/types.hpp"

#include <memory>

namespace rsx::remix
{
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
		void present(display_handle_t window_handle, u32 width, u32 height);
		void shutdown();

	private:
		struct impl;
		std::unique_ptr<impl> m_impl;
	};
}
