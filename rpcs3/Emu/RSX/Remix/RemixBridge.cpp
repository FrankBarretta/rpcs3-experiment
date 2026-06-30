#include "stdafx.h"
#include "RemixBridge.h"

#include "RemixApi.h"
#include "Emu/system_config.h"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"
#include "Utilities/StrUtil.h"

#ifdef _WIN32
#include "util/dyn_lib.hpp"
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace rsx::remix
{
	namespace
	{
#ifdef _WIN32
		constexpr wchar_t remix_window_class_name[] = L"RPCS3RemixOutputWindow";

		LRESULT CALLBACK remix_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
		{
			switch (message)
			{
			case WM_NCHITTEST:
				return HTTRANSPARENT;
			case WM_ERASEBKGND:
				return 1;
			default:
				return DefWindowProcW(hwnd, message, wparam, lparam);
			}
		}

		bool ensure_remix_window_class()
		{
			static const bool registered = []() -> bool
			{
				WNDCLASSW wc{};
				wc.lpfnWndProc = remix_window_proc;
				wc.hInstance = GetModuleHandleW(nullptr);
				wc.lpszClassName = remix_window_class_name;
				wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

				return RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
			}();

			return registered;
		}

		void resize_remix_child_window(HWND child, HWND parent)
		{
			if (!child || !parent)
			{
				return;
			}

			RECT rect{};
			if (!GetClientRect(parent, &rect))
			{
				return;
			}

			const int width = std::max<LONG>(1, rect.right - rect.left);
			const int height = std::max<LONG>(1, rect.bottom - rect.top);
			SetWindowPos(child, HWND_TOP, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
		}

		HWND create_remix_child_window(HWND parent)
		{
			if (!parent || !ensure_remix_window_class())
			{
				return nullptr;
			}

			RECT rect{};
			GetClientRect(parent, &rect);

			const int width = std::max<LONG>(1, rect.right - rect.left);
			const int height = std::max<LONG>(1, rect.bottom - rect.top);

			const HWND child = CreateWindowExW(
				WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
				remix_window_class_name,
				L"",
				WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
				0,
				0,
				width,
				height,
				parent,
				nullptr,
				GetModuleHandleW(nullptr),
				nullptr);

			if (child)
			{
				resize_remix_child_window(child, parent);
			}

			return child;
		}
#endif

		bool is_absolute_path(const std::string& path)
		{
			if (path.empty())
			{
				return false;
			}

			if (path[0] == '/' || path[0] == '\\')
			{
				return true;
			}

			return path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
		}

		std::string resolve_runtime_path(const std::string& dll_path)
		{
			if (dll_path.empty() || is_absolute_path(dll_path))
			{
				return dll_path;
			}

			const std::string exe_dir = fs::get_executable_dir();
			return exe_dir.empty() ? dll_path : exe_dir + dll_path;
		}

		std::string make_trex_runtime_path(const std::string& dll_path)
		{
			const usz slash = dll_path.find_last_of("/\\");
			if (slash == umax)
			{
				return ".trex/d3d9.dll";
			}

			return dll_path.substr(0, slash + 1) + ".trex/d3d9.dll";
		}

		std::string get_parent_path(const std::string& path)
		{
			const usz slash = path.find_last_of("/\\");
			return slash == umax ? std::string{} : path.substr(0, slash + 1);
		}

		std::string make_particle_system_path(const std::string& dll_path)
		{
			const std::string runtime_dir = get_parent_path(dll_path);
			return runtime_dir.empty() ? std::string{} : runtime_dir + "usd/plugins/RemixParticleSystem/RemixParticleSystem.dll";
		}

		std::string make_particle_system_directory(const std::string& dll_path)
		{
			const std::string runtime_dir = get_parent_path(dll_path);
			return runtime_dir.empty() ? std::string{} : runtime_dir + "usd/plugins/RemixParticleSystem";
		}

		const char* remix_error_to_string(api::remixapi_ErrorCode status)
		{
			using namespace api;

			switch (status)
			{
			case REMIXAPI_ERROR_CODE_SUCCESS: return "success";
			case REMIXAPI_ERROR_CODE_GENERAL_FAILURE: return "general failure";
			case REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE: return "load library failure";
			case REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS: return "invalid arguments";
			case REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE: return "missing exported function";
			case REMIXAPI_ERROR_CODE_ALREADY_EXISTS: return "already exists";
			case REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D9_DEVICE: return "non-Remix D3D9 device";
			case REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED: return "Remix device was not registered";
			case REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION: return "incompatible version";
			case REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE: return "SetDllDirectory failure";
			case REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE: return "GetFullPathName failure";
			case REMIXAPI_ERROR_CODE_NOT_INITIALIZED: return "not initialized";
			case REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES: return "GPU features missing";
			case REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM: return "driver version below minimum";
			case REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL: return "DXVK instance extension failure";
			case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL: return "Vulkan instance creation failure";
			case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL: return "Vulkan device creation failure";
			case REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING: return "graphics queue family missing";
			default: return "unknown";
			}
		}

		api::remixapi_HardcodedVertex make_debug_vertex(float x, float y, float z)
		{
			api::remixapi_HardcodedVertex result{};
			result.position[0] = x;
			result.position[1] = y;
			result.position[2] = z;
			result.normal[2] = -1.0f;
			result.color = 0xffffffffu;
			return result;
		}

		api::remixapi_Transform make_identity_transform(float z)
		{
			api::remixapi_Transform result{};
			result.matrix[0][0] = 1.0f;
			result.matrix[1][1] = 1.0f;
			result.matrix[2][2] = 1.0f;
			result.matrix[2][3] = z;
			return result;
		}

		api::remixapi_Transform make_identity_transform()
		{
			api::remixapi_Transform result{};
			result.matrix[0][0] = 1.0f;
			result.matrix[1][1] = 1.0f;
			result.matrix[2][2] = 1.0f;
			return result;
		}

		constexpr u32 max_remix_meshes_per_frame = 64;
		constexpr usz max_remix_mesh_vertices_per_frame = 100000;
		constexpr usz max_remix_mesh_indices_per_frame = 300000;
		constexpr usz max_remix_registered_meshes = 512;
		constexpr float max_remix_abs_position = 1000000.0f;
		constexpr float max_remix_mesh_extent = 1000000.0f;
	}

	struct bridge::impl
	{
#ifdef _WIN32
		utils::dynamic_library runtime;
		utils::dynamic_library particle_system;
		std::vector<DLL_DIRECTORY_COOKIE> dll_directory_cookies;
		HWND present_window = nullptr;
		bool using_external_swapchain = false;
#endif
		api::remixapi_Interface api_table{};
		api::remixapi_MeshHandle debug_mesh = nullptr;
		api::remixapi_LightHandle debug_light = nullptr;
		std::vector<api::remixapi_MeshHandle> registered_meshes;

		bool initialized = false;
		bool frame_active = false;
		bool scene_created = false;
		bool logged_draw_hook = false;
		bool logged_mesh_bridge = false;
		bool logged_mesh_budget = false;
		bool logged_mesh_registry_full = false;
		bool logged_mesh_sanity = false;
		bool logged_mesh_stats = false;
		bool logged_unsupported = false;
		bool logged_present_disabled = false;
		bool logged_external_output_unwired = false;
		bool frame_bounds_valid = false;
		std::array<float, 3> frame_bounds_min =
		{
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()
		};
		std::array<float, 3> frame_bounds_max =
		{
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest()
		};
		u64 frame_index = 0;
		u32 draws_this_frame = 0;
		u32 indexed_draws_this_frame = 0;
		u32 vertices_this_frame = 0;
		u32 meshes_this_frame = 0;
		usz mesh_vertices_this_frame = 0;
		usz mesh_indices_this_frame = 0;
		u32 rejected_meshes_this_frame = 0;

		bool validate_api_table() const
		{
			return api_table.Startup &&
				api_table.Shutdown &&
				api_table.Present &&
				api_table.SetupCamera &&
				api_table.CreateMesh &&
				api_table.DestroyMesh &&
				api_table.DrawInstance &&
				api_table.CreateLight &&
				api_table.DestroyLight &&
				api_table.DrawLightInstance;
		}

		void destroy_registered_meshes()
		{
			if (api_table.DestroyMesh)
			{
				for (const auto mesh : registered_meshes)
				{
					api_table.DestroyMesh(mesh);
				}
			}

			registered_meshes.clear();
		}

		void reset_frame_bounds()
		{
			frame_bounds_valid = false;
			frame_bounds_min =
			{
				std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max()
			};
			frame_bounds_max =
			{
				std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest(),
				std::numeric_limits<float>::lowest()
			};
		}

		void include_bounds(float x, float y, float z)
		{
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
			{
				return;
			}

			frame_bounds_valid = true;
			frame_bounds_min[0] = std::min(frame_bounds_min[0], x);
			frame_bounds_min[1] = std::min(frame_bounds_min[1], y);
			frame_bounds_min[2] = std::min(frame_bounds_min[2], z);
			frame_bounds_max[0] = std::max(frame_bounds_max[0], x);
			frame_bounds_max[1] = std::max(frame_bounds_max[1], y);
			frame_bounds_max[2] = std::max(frame_bounds_max[2], z);
		}

		bool setup_camera(u32 width, u32 height, bool auto_fit)
		{
			const float safe_width = static_cast<float>(std::max<u32>(width, 1));
			const float safe_height = static_cast<float>(std::max<u32>(height, 1));

			std::array<float, 3> center = { 0.0f, 0.0f, 0.0f };
			float radius = 10.0f;

			if (auto_fit && frame_bounds_valid)
			{
				center =
				{
					(frame_bounds_min[0] + frame_bounds_max[0]) * 0.5f,
					(frame_bounds_min[1] + frame_bounds_max[1]) * 0.5f,
					(frame_bounds_min[2] + frame_bounds_max[2]) * 0.5f
				};

				const float ex = frame_bounds_max[0] - frame_bounds_min[0];
				const float ey = frame_bounds_max[1] - frame_bounds_min[1];
				const float ez = frame_bounds_max[2] - frame_bounds_min[2];
				radius = std::sqrt(ex * ex + ey * ey + ez * ez) * 0.5f;
				radius = std::clamp(radius, 0.5f, 100000.0f);
			}

			constexpr float fov_y = 70.0f;
			constexpr float tan_half_fov = 0.7002075f;
			const float distance = std::clamp((radius / tan_half_fov) + radius, 2.0f, 250000.0f);

			api::remixapi_CameraInfoParameterizedEXT camera_params{};
			camera_params.sType = api::REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
			camera_params.position = { center[0], center[1], center[2] - distance };
			camera_params.forward = { 0.0f, 0.0f, 1.0f };
			camera_params.up = { 0.0f, 1.0f, 0.0f };
			camera_params.right = { 1.0f, 0.0f, 0.0f };
			camera_params.fovYInDegrees = fov_y;
			camera_params.aspect = safe_width / safe_height;
			camera_params.nearPlane = std::max(0.001f, radius * 0.001f);
			camera_params.farPlane = std::max(1000.0f, distance + radius * 8.0f);

			api::remixapi_CameraInfo camera_info{};
			camera_info.sType = api::REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
			camera_info.pNext = &camera_params;
			camera_info.type = api::REMIXAPI_CAMERA_TYPE_WORLD;

			if (const auto status = api_table.SetupCamera(&camera_info);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: SetupCamera failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
				return false;
			}

			return true;
		}

		void submit_frame_light()
		{
			if (!api_table.CreateLight || !api_table.DestroyLight || !api_table.DrawLightInstance)
			{
				return;
			}

			if (debug_light)
			{
				api_table.DrawLightInstance(debug_light);
				return;
			}

			if (!frame_bounds_valid)
			{
				return;
			}

			std::array<float, 3> center = { 0.0f, -1.0f, 0.0f };
			float radius = 10.0f;

			if (frame_bounds_valid)
			{
				center =
				{
					(frame_bounds_min[0] + frame_bounds_max[0]) * 0.5f,
					(frame_bounds_min[1] + frame_bounds_max[1]) * 0.5f,
					(frame_bounds_min[2] + frame_bounds_max[2]) * 0.5f
				};

				const float ex = frame_bounds_max[0] - frame_bounds_min[0];
				const float ey = frame_bounds_max[1] - frame_bounds_min[1];
				const float ez = frame_bounds_max[2] - frame_bounds_min[2];
				radius = std::clamp(std::sqrt(ex * ex + ey * ey + ez * ez) * 0.5f, 0.5f, 100000.0f);
			}

			api::remixapi_LightInfoSphereEXT sphere_light{};
			sphere_light.sType = api::REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			sphere_light.position = { center[0], center[1] - radius, center[2] - radius * 1.5f };
			sphere_light.radius = std::max(0.1f, radius * 0.02f);

			api::remixapi_LightInfo light_info{};
			light_info.sType = api::REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			light_info.pNext = &sphere_light;
			light_info.hash = 0x5250435333524c31ull;
			light_info.radiance = { radius * 3000.0f, radius * 3000.0f, radius * 3000.0f };

			if (const auto status = api_table.CreateLight(&light_info, &debug_light);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: CreateLight failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
				return;
			}

			api_table.DrawLightInstance(debug_light);
		}

		void reset_runtime()
		{
#ifdef _WIN32
			if (present_window)
			{
				DestroyWindow(present_window);
				present_window = nullptr;
			}
			using_external_swapchain = false;

			particle_system.close();

			for (const auto cookie : dll_directory_cookies)
			{
				RemoveDllDirectory(cookie);
			}
			dll_directory_cookies.clear();
#endif
			destroy_registered_meshes();
			api_table = {};
			debug_mesh = nullptr;
			debug_light = nullptr;
			initialized = false;
			frame_active = false;
			scene_created = false;
			reset_frame_bounds();
			draws_this_frame = 0;
			indexed_draws_this_frame = 0;
			vertices_this_frame = 0;
			meshes_this_frame = 0;
			mesh_vertices_this_frame = 0;
			mesh_indices_this_frame = 0;
			rejected_meshes_this_frame = 0;
		}

#ifdef _WIN32
		bool add_dll_directory(const std::string& path)
		{
			if (path.empty() || !fs::is_dir(path))
			{
				return false;
			}

			if (const auto cookie = AddDllDirectory(utf8_to_wchar(path).c_str()))
			{
				dll_directory_cookies.push_back(cookie);
				return true;
			}

			rsx_log.warning("RTX Remix: AddDllDirectory failed for '%s': %s.", path, fmt::win_error{GetLastError(), nullptr});
			return false;
		}
#endif
	};

	bridge::bridge()
		: m_impl(std::make_unique<impl>())
	{
	}

	bridge::~bridge()
	{
		shutdown();
	}

	bool bridge::is_enabled() const
	{
		return g_cfg.video.rtx_remix.enabled.get();
	}

	bool bridge::is_running() const
	{
		return m_impl && m_impl->initialized;
	}

	void bridge::initialize(display_handle_t window_handle)
	{
		if (!m_impl || !is_enabled() || m_impl->initialized)
		{
			return;
		}

#ifdef _WIN32
		if (!window_handle)
		{
			rsx_log.warning("RTX Remix: cannot start runtime without a window handle.");
			return;
		}

		std::string dll_path = g_cfg.video.rtx_remix.runtime_dll_path.to_string();
		if (dll_path.empty())
		{
			dll_path = "d3d9.dll";
		}
		dll_path = resolve_runtime_path(dll_path);

		constexpr DWORD remix_load_flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

		auto prepare_runtime_dependencies = [this](const std::string& path) -> bool
		{
			m_impl->add_dll_directory(get_parent_path(path));
			m_impl->add_dll_directory(make_particle_system_directory(path));

			const std::string particle_system_path = make_particle_system_path(path);
			if (!m_impl->particle_system.loaded() && !particle_system_path.empty() && fs::is_file(particle_system_path))
			{
				SetLastError(ERROR_SUCCESS);
				if (!m_impl->particle_system.load_with_search_flags(utf8_to_wchar(particle_system_path), remix_load_flags))
				{
					const DWORD error = GetLastError();
					rsx_log.error("RTX Remix: failed to preload particle plugin '%s': %s.", particle_system_path, fmt::win_error{error, nullptr});
					m_impl->reset_runtime();
					return false;
				}
			}

			return true;
		};

		if (!prepare_runtime_dependencies(dll_path))
		{
			return;
		}

		auto load_runtime = [this](const std::string& path, DWORD& error)
		{
			SetLastError(ERROR_SUCCESS);
			if (m_impl->runtime.load_with_search_flags(utf8_to_wchar(path), remix_load_flags))
			{
				return true;
			}

			error = GetLastError();
			return false;
		};

		DWORD load_error = ERROR_SUCCESS;
		if (!load_runtime(dll_path, load_error))
		{
			if (load_error == ERROR_BAD_EXE_FORMAT)
			{
				rsx_log.error("RTX Remix: runtime DLL architecture mismatch. Use the x64 renderer DLL, usually '.trex/d3d9.dll', not the 32-bit bridge d3d9.dll.");

				const std::string trex_dll_path = make_trex_runtime_path(dll_path);
				if (trex_dll_path != dll_path)
				{
					rsx_log.notice("RTX Remix: trying x64 renderer DLL '%s'.", trex_dll_path);

					DWORD trex_load_error = ERROR_SUCCESS;
					if (prepare_runtime_dependencies(trex_dll_path) && load_runtime(trex_dll_path, trex_load_error))
					{
						dll_path = trex_dll_path;
					}
					else
					{
						rsx_log.error("RTX Remix: failed to load x64 renderer DLL '%s': %s.", trex_dll_path, fmt::win_error{trex_load_error, nullptr});
						m_impl->reset_runtime();
						return;
					}
				}
				else
				{
					rsx_log.error("RTX Remix: failed to load runtime DLL '%s': %s.", dll_path, fmt::win_error{load_error, nullptr});
					m_impl->reset_runtime();
					return;
				}
			}
			else
			{
				rsx_log.error("RTX Remix: failed to load runtime DLL '%s': %s.", dll_path, fmt::win_error{load_error, nullptr});
				m_impl->reset_runtime();
				return;
			}
		}

		const auto initialize_library = m_impl->runtime.get<api::PFN_remixapi_InitializeLibrary>("remixapi_InitializeLibrary");
		if (!initialize_library)
		{
			rsx_log.error("RTX Remix: '%s' does not export remixapi_InitializeLibrary.", dll_path);
			m_impl->runtime.close();
			m_impl->reset_runtime();
			return;
		}

		api::remixapi_InitializeLibraryInfo library_info{};
		library_info.sType = api::REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
		library_info.version = RPCS3_REMIXAPI_VERSION_MAKE(
			RPCS3_REMIXAPI_VERSION_MAJOR,
			RPCS3_REMIXAPI_VERSION_MINOR,
			RPCS3_REMIXAPI_VERSION_PATCH);

		if (const auto status = initialize_library(&library_info, &m_impl->api_table);
			status != api::REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.error("RTX Remix: remixapi_InitializeLibrary failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
			m_impl->runtime.close();
			m_impl->reset_runtime();
			return;
		}

		if (!m_impl->validate_api_table())
		{
			rsx_log.error("RTX Remix: runtime API table is incomplete.");
			m_impl->runtime.close();
			m_impl->reset_runtime();
			return;
		}

		api::remixapi_StartupInfo startup_info{};
		startup_info.sType = api::REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
		startup_info.hwnd = reinterpret_cast<api::remixapi_HWND>(window_handle);
		startup_info.disableSrgbConversionForOutput = false;
		startup_info.forceNoVkSwapchain = true;
		startup_info.editorModeEnabled = g_cfg.video.rtx_remix.editor_mode.get();

#ifdef _WIN32
		if (g_cfg.video.rtx_remix.present_output.get())
		{
			m_impl->present_window = create_remix_child_window(reinterpret_cast<HWND>(window_handle));
			if (m_impl->present_window)
			{
				startup_info.hwnd = reinterpret_cast<api::remixapi_HWND>(m_impl->present_window);
				startup_info.forceNoVkSwapchain = false;
				rsx_log.notice("RTX Remix: using a dedicated child window for the runtime swapchain.");
			}
			else
			{
				rsx_log.warning("RTX Remix: could not create a child output window; starting in external no-swapchain mode.");
			}
		}

		m_impl->using_external_swapchain = startup_info.forceNoVkSwapchain;
#endif

		if (const auto status = m_impl->api_table.Startup(&startup_info);
			status != api::REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.error("RTX Remix: Startup failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
			m_impl->runtime.close();
			m_impl->reset_runtime();
			return;
		}

		m_impl->initialized = true;
		rsx_log.success("RTX Remix: runtime started from '%s'.", dll_path);
#else
		if (!m_impl->logged_unsupported)
		{
			rsx_log.warning("RTX Remix: runtime bridge is only available on Windows.");
			m_impl->logged_unsupported = true;
		}
#endif
	}

	void bridge::begin_frame(u32 width, u32 height)
	{
		if (!m_impl || !m_impl->initialized || m_impl->frame_active)
		{
			return;
		}

		// Reuse the previous frame's bounds before collecting the new frame.
		// Remix command ordering makes this more reliable than setting the camera
		// only after all current-frame instances have already been submitted.
		m_impl->setup_camera(width, height, m_impl->frame_bounds_valid);
		m_impl->reset_frame_bounds();
		m_impl->frame_active = true;
		m_impl->draws_this_frame = 0;
		m_impl->indexed_draws_this_frame = 0;
		m_impl->vertices_this_frame = 0;
		m_impl->meshes_this_frame = 0;
		m_impl->mesh_vertices_this_frame = 0;
		m_impl->mesh_indices_this_frame = 0;
		m_impl->rejected_meshes_this_frame = 0;

		if (!g_cfg.video.rtx_remix.debug_triangle.get())
		{
			return;
		}

		if (!m_impl->scene_created)
		{
			api::remixapi_LightInfoSphereEXT sphere_light{};
			sphere_light.sType = api::REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
			sphere_light.position = { 0.0f, -1.0f, 0.0f };
			sphere_light.radius = 0.1f;

			api::remixapi_LightInfo light_info{};
			light_info.sType = api::REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			light_info.pNext = &sphere_light;
			light_info.hash = 0x5250435333524c31ull;
			light_info.radiance = { 100.0f, 200.0f, 100.0f };

			if (const auto status = m_impl->api_table.CreateLight(&light_info, &m_impl->debug_light);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: CreateLight failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
			}

			const api::remixapi_HardcodedVertex vertices[] =
			{
				make_debug_vertex( 5.0f, -5.0f, 10.0f),
				make_debug_vertex( 0.0f,  5.0f, 10.0f),
				make_debug_vertex(-5.0f, -5.0f, 10.0f),
			};

			api::remixapi_MeshInfoSurfaceTriangles triangles{};
			triangles.vertices_values = vertices;
			triangles.vertices_count = sizeof(vertices) / sizeof(vertices[0]);

			api::remixapi_MeshInfo mesh_info{};
			mesh_info.sType = api::REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mesh_info.hash = 0x52504353334d5331ull;
			mesh_info.surfaces_values = &triangles;
			mesh_info.surfaces_count = 1;

			if (const auto status = m_impl->api_table.CreateMesh(&mesh_info, &m_impl->debug_mesh);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: CreateMesh failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
			}

			m_impl->scene_created = true;
		}

		if (m_impl->debug_mesh)
		{
			api::remixapi_InstanceInfo instance_info{};
			instance_info.sType = api::REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
			instance_info.mesh = m_impl->debug_mesh;
			instance_info.transform = make_identity_transform(5.0f);
			instance_info.doubleSided = true;

			if (const auto status = m_impl->api_table.DrawInstance(&instance_info);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: DrawInstance failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
			}
		}

		if (m_impl->debug_light)
		{
			m_impl->api_table.DrawLightInstance(m_impl->debug_light);
		}
	}

	void bridge::note_rsx_draw(u32 vertex_count, bool indexed, u32 instance_count)
	{
		if (!m_impl || !m_impl->initialized)
		{
			return;
		}

		m_impl->draws_this_frame++;
		m_impl->vertices_this_frame += vertex_count * std::max<u32>(instance_count, 1);
		if (indexed)
		{
			m_impl->indexed_draws_this_frame++;
		}

		if (!m_impl->logged_draw_hook)
		{
			rsx_log.notice("RTX Remix: RSX draw hook is active; full RSX mesh/material conversion is the next integration step.");
			m_impl->logged_draw_hook = true;
		}
	}

	void bridge::submit_mesh(const mesh_capture& mesh)
	{
		if (!m_impl || !m_impl->initialized || !m_impl->frame_active)
		{
			return;
		}

		if (mesh.vertices.size() < 3 || mesh.indices.size() < 3)
		{
			return;
		}

		if (m_impl->meshes_this_frame >= max_remix_meshes_per_frame ||
			m_impl->mesh_vertices_this_frame + mesh.vertices.size() > max_remix_mesh_vertices_per_frame ||
			m_impl->mesh_indices_this_frame + mesh.indices.size() > max_remix_mesh_indices_per_frame)
		{
			if (!m_impl->logged_mesh_budget)
			{
				rsx_log.warning("RTX Remix: per-frame RSX mesh budget reached; dropping remaining meshes this frame.");
				m_impl->logged_mesh_budget = true;
			}

			return;
		}

		std::array<float, 3> local_min =
		{
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max(),
			std::numeric_limits<float>::max()
		};
		std::array<float, 3> local_max =
		{
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest(),
			std::numeric_limits<float>::lowest()
		};

		for (const auto& src : mesh.vertices)
		{
			if (!std::isfinite(src.x) || !std::isfinite(src.y) || !std::isfinite(src.z) ||
				std::abs(src.x) > max_remix_abs_position ||
				std::abs(src.y) > max_remix_abs_position ||
				std::abs(src.z) > max_remix_abs_position)
			{
				m_impl->rejected_meshes_this_frame++;
				if (!m_impl->logged_mesh_sanity)
				{
					rsx_log.warning("RTX Remix: dropping RSX meshes with invalid or extreme raw ATTR0 positions.");
					m_impl->logged_mesh_sanity = true;
				}
				return;
			}

			local_min[0] = std::min(local_min[0], src.x);
			local_min[1] = std::min(local_min[1], src.y);
			local_min[2] = std::min(local_min[2], src.z);
			local_max[0] = std::max(local_max[0], src.x);
			local_max[1] = std::max(local_max[1], src.y);
			local_max[2] = std::max(local_max[2], src.z);
		}

		const float extent_x = local_max[0] - local_min[0];
		const float extent_y = local_max[1] - local_min[1];
		const float extent_z = local_max[2] - local_min[2];
		if (!std::isfinite(extent_x) || !std::isfinite(extent_y) || !std::isfinite(extent_z) ||
			extent_x > max_remix_mesh_extent ||
			extent_y > max_remix_mesh_extent ||
			extent_z > max_remix_mesh_extent)
		{
			m_impl->rejected_meshes_this_frame++;
			if (!m_impl->logged_mesh_sanity)
			{
				rsx_log.warning("RTX Remix: dropping RSX meshes with extreme raw ATTR0 bounds.");
				m_impl->logged_mesh_sanity = true;
			}
			return;
		}

		for (const u32 index : mesh.indices)
		{
			if (index >= mesh.vertices.size())
			{
				return;
			}
		}

		m_impl->include_bounds(local_min[0], local_min[1], local_min[2]);
		m_impl->include_bounds(local_max[0], local_max[1], local_max[2]);

		u64 hash = mesh.hash;
		if (!hash)
		{
			hash = 0x52504353334d0001ull;
		}

		api::remixapi_MeshHandle handle = reinterpret_cast<api::remixapi_MeshHandle>(hash);
		const bool already_registered =
			std::find(m_impl->registered_meshes.begin(), m_impl->registered_meshes.end(), handle) != m_impl->registered_meshes.end();

		if (!already_registered)
		{
			if (m_impl->registered_meshes.size() >= max_remix_registered_meshes)
			{
				if (!m_impl->logged_mesh_registry_full)
				{
					rsx_log.warning("RTX Remix: RSX mesh registry limit reached; dropping new unique meshes.");
					m_impl->logged_mesh_registry_full = true;
				}

				return;
			}

			std::vector<api::remixapi_HardcodedVertex> vertices;
			vertices.reserve(mesh.vertices.size());

			for (const auto& src : mesh.vertices)
			{
				api::remixapi_HardcodedVertex dst{};
				dst.position[0] = src.x;
				dst.position[1] = src.y;
				dst.position[2] = src.z;
				dst.normal[2] = -1.0f;
				dst.color = 0xffffffffu;
				vertices.push_back(dst);
			}

			api::remixapi_MeshInfoSurfaceTriangles triangles{};
			triangles.vertices_values = vertices.data();
			triangles.vertices_count = vertices.size();
			triangles.indices_values = mesh.indices.data();
			triangles.indices_count = mesh.indices.size();

			api::remixapi_MeshInfo mesh_info{};
			mesh_info.sType = api::REMIXAPI_STRUCT_TYPE_MESH_INFO;
			mesh_info.hash = hash;
			mesh_info.surfaces_values = &triangles;
			mesh_info.surfaces_count = 1;

			api::remixapi_MeshHandle created_handle = nullptr;
			if (const auto status = m_impl->api_table.CreateMesh(&mesh_info, &created_handle);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: CreateMesh for RSX draw failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
				return;
			}

			handle = created_handle;
			m_impl->registered_meshes.push_back(handle);
		}

		api::remixapi_InstanceInfo instance_info{};
		instance_info.sType = api::REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		instance_info.mesh = handle;
		instance_info.transform = make_identity_transform();
		instance_info.doubleSided = true;

		if (const auto status = m_impl->api_table.DrawInstance(&instance_info);
			status != api::REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.warning("RTX Remix: DrawInstance for RSX draw failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
		}

		m_impl->meshes_this_frame++;
		m_impl->mesh_vertices_this_frame += mesh.vertices.size();
		m_impl->mesh_indices_this_frame += mesh.indices.size();

		if (!m_impl->logged_mesh_bridge)
		{
			rsx_log.notice("RTX Remix: RSX mesh bridge is submitting triangle meshes with raw ATTR0 positions.");
			m_impl->logged_mesh_bridge = true;
		}
	}

	void bridge::present(display_handle_t window_handle, u32 width, u32 height)
	{
		if (!m_impl || !m_impl->initialized)
		{
			return;
		}

		if (!m_impl->frame_active)
		{
			begin_frame(width, height);
		}

		if (!g_cfg.video.rtx_remix.present_output.get())
		{
			if (!m_impl->logged_present_disabled)
			{
				rsx_log.notice("RTX Remix: runtime is active, but Present Output is disabled.");
				m_impl->logged_present_disabled = true;
			}
			m_impl->frame_active = false;
			return;
		}

#ifdef _WIN32
		m_impl->setup_camera(width, height, m_impl->meshes_this_frame > 0);
		m_impl->submit_frame_light();

		if (!m_impl->logged_mesh_stats || (m_impl->frame_index && (m_impl->frame_index % 60) == 0))
		{
			rsx_log.notice("RTX Remix: frame stats: meshes=%u, registered=%zu, rejected=%u, draws=%u.",
				m_impl->meshes_this_frame,
				m_impl->registered_meshes.size(),
				m_impl->rejected_meshes_this_frame,
				m_impl->draws_this_frame);
			m_impl->logged_mesh_stats = true;
		}

		if (m_impl->using_external_swapchain)
		{
			if (!m_impl->logged_external_output_unwired)
			{
				rsx_log.notice("RTX Remix: external no-swapchain output is active, but Vulkan image handoff is not wired yet; skipping runtime Present.");
				m_impl->logged_external_output_unwired = true;
			}

			m_impl->frame_active = false;
			m_impl->frame_index++;
			return;
		}

		if (m_impl->present_window)
		{
			resize_remix_child_window(m_impl->present_window, reinterpret_cast<HWND>(window_handle));
		}

		api::remixapi_PresentInfo present_info{};
		present_info.sType = api::REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
		present_info.hwndOverride = reinterpret_cast<api::remixapi_HWND>(m_impl->present_window ? m_impl->present_window : reinterpret_cast<HWND>(window_handle));

		if (const auto status = m_impl->api_table.Present(&present_info);
			status != api::REMIXAPI_ERROR_CODE_SUCCESS)
		{
			rsx_log.warning("RTX Remix: Present failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
		}
#else
		(void)window_handle;
#endif

		m_impl->frame_active = false;
		m_impl->frame_index++;
	}

	void bridge::shutdown()
	{
		if (!m_impl || !m_impl->initialized)
		{
			return;
		}

		m_impl->destroy_registered_meshes();

		if (m_impl->debug_mesh && m_impl->api_table.DestroyMesh)
		{
			m_impl->api_table.DestroyMesh(m_impl->debug_mesh);
			m_impl->debug_mesh = nullptr;
		}

		if (m_impl->debug_light && m_impl->api_table.DestroyLight)
		{
			m_impl->api_table.DestroyLight(m_impl->debug_light);
			m_impl->debug_light = nullptr;
		}

		if (m_impl->api_table.Shutdown)
		{
			if (const auto status = m_impl->api_table.Shutdown();
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: Shutdown failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
			}
		}

#ifdef _WIN32
		m_impl->runtime.close();
#endif
		m_impl->reset_runtime();
		rsx_log.notice("RTX Remix: runtime stopped.");
	}
}
