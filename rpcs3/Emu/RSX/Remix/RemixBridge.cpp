#include "stdafx.h"
#include "RemixBridge.h"

#include "RemixApi.h"
#include "RemixTexture.h"
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
#include <string>
#include <unordered_map>
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

		// ---- Game-camera reconstruction helpers (see impl::setup_camera_from_view_proj) ----

		std::array<float, 3> v3_sub(const std::array<float, 3>& a, const std::array<float, 3>& b)
		{
			return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
		}

		float v3_dot(const std::array<float, 3>& a, const std::array<float, 3>& b)
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		float v3_length(const std::array<float, 3>& a)
		{
			return std::sqrt(v3_dot(a, a));
		}

		bool v3_normalize(std::array<float, 3>& a)
		{
			const float length = v3_length(a);
			if (!std::isfinite(length) || length < 1e-6f)
			{
				return false;
			}

			a = { a[0] / length, a[1] / length, a[2] / length };
			return true;
		}

		// Row-major 4x4 inverse via cofactor expansion (the classic MESA/GLU routine),
		// computed in double to keep precision on projection-scale coefficients.
		bool invert_mat4(const float in[16], float out[16])
		{
			double m[16];
			for (u32 i = 0; i < 16; i++)
			{
				m[i] = in[i];
			}

			double inv[16];
			inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
			inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
			inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
			inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
			inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
			inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
			inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
			inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
			inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
			inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
			inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
			inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
			inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
			inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
			inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
			inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

			const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
			if (!std::isfinite(det) || std::abs(det) < 1e-35)
			{
				return false;
			}

			const double inv_det = 1.0 / det;
			for (u32 i = 0; i < 16; i++)
			{
				out[i] = static_cast<float>(inv[i] * inv_det);
				if (!std::isfinite(out[i]))
				{
					return false;
				}
			}

			return true;
		}

		bool unproject_ndc(const float inv_vp[16], float x, float y, float z, std::array<float, 3>& out)
		{
			float h[4];
			for (u32 row = 0; row < 4; row++)
			{
				h[row] = inv_vp[row * 4 + 0] * x + inv_vp[row * 4 + 1] * y + inv_vp[row * 4 + 2] * z + inv_vp[row * 4 + 3];
			}

			if (!std::isfinite(h[3]) || std::abs(h[3]) < 1e-9f)
			{
				return false;
			}

			out = { h[0] / h[3], h[1] / h[3], h[2] / h[3] };
			return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
		}

		// Least-squares intersection point of a set of rays: solve sum(I - d*d^T) x = sum((I - d*d^T) p).
		// Near-parallel rays (an ortho projection) make the system singular, which is the desired
		// rejection path since an ortho camera has no eye to converge on.
		bool converge_rays(const std::array<std::array<float, 3>, 4>& origins, const std::array<std::array<float, 3>, 4>& dirs, std::array<float, 3>& out)
		{
			double a[3][3] = {};
			double b[3] = {};

			for (u32 i = 0; i < 4; i++)
			{
				const double length = std::sqrt(
					static_cast<double>(dirs[i][0]) * dirs[i][0] +
					static_cast<double>(dirs[i][1]) * dirs[i][1] +
					static_cast<double>(dirs[i][2]) * dirs[i][2]);

				if (!(length > 1e-9))
				{
					return false;
				}

				const double d[3] = { dirs[i][0] / length, dirs[i][1] / length, dirs[i][2] / length };

				for (u32 row = 0; row < 3; row++)
				{
					for (u32 col = 0; col < 3; col++)
					{
						const double proj = (row == col ? 1.0 : 0.0) - d[row] * d[col];
						a[row][col] += proj;
						b[row] += proj * origins[i][col];
					}
				}
			}

			const auto det3 = [](double a0, double a1, double a2, double b0, double b1, double b2, double c0, double c1, double c2)
			{
				return a0 * (b1 * c2 - b2 * c1) - a1 * (b0 * c2 - b2 * c0) + a2 * (b0 * c1 - b1 * c0);
			};

			const double det = det3(a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2]);
			if (!std::isfinite(det) || std::abs(det) < 1e-9)
			{
				return false;
			}

			out[0] = static_cast<float>(det3(b[0], a[0][1], a[0][2], b[1], a[1][1], a[1][2], b[2], a[2][1], a[2][2]) / det);
			out[1] = static_cast<float>(det3(a[0][0], b[0], a[0][2], a[1][0], b[1], a[1][2], a[2][0], b[2], a[2][2]) / det);
			out[2] = static_cast<float>(det3(a[0][0], a[0][1], b[0], a[1][0], a[1][1], b[1], a[2][0], a[2][1], b[2]) / det);

			return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
		}

		// Generous internal safety ceilings. The per-frame mesh count and the mesh/material
		// registry sizes are user-configurable (g_cfg.video.rtx_remix.*) so that large scenes
		// can push far more geometry to Remix than the original fixed 64-mesh / 512-entry limits.
		constexpr usz max_remix_mesh_vertices_per_frame = 16000000;
		constexpr usz max_remix_mesh_indices_per_frame = 48000000;
		constexpr float max_remix_abs_position = 10000000.0f;
		constexpr float max_remix_mesh_extent = 10000000.0f;

		usz cfg_max_meshes_per_frame()
		{
			return g_cfg.video.rtx_remix.max_meshes_per_frame.get();
		}

		usz cfg_max_registered_meshes()
		{
			return g_cfg.video.rtx_remix.max_registered_meshes.get();
		}

		usz cfg_max_registered_materials()
		{
			return g_cfg.video.rtx_remix.max_registered_materials.get();
		}
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
		// Per-frame scene light (headlight or bounds fallback). Kept separate from debug_light,
		// which is reserved for the debug triangle.
		api::remixapi_LightHandle frame_light = nullptr;
		std::unordered_map<u64, api::remixapi_MeshHandle> registered_meshes;
		std::unordered_map<u64, api::remixapi_MaterialHandle> registered_materials;

		bool initialized = false;
		bool frame_active = false;
		bool scene_created = false;
		bool logged_draw_hook = false;
		bool logged_mesh_bridge = false;
		bool logged_mesh_budget = false;
		bool logged_mesh_registry_full = false;
		bool logged_material_registry_full = false;
		bool logged_material_bridge = false;
		bool logged_mesh_sanity = false;
		bool logged_mesh_stats = false;
		bool logged_unsupported = false;
		bool logged_present_disabled = false;
		bool logged_external_output_unwired = false;
		bool logged_game_camera = false;
		// Row-major world->clip view-projection captured from the game's draws.
		// view_proj_valid means "some frame captured one" (it survives frames with no capture so
		// the camera stays stable); view_proj_fresh means "captured during the current frame".
		float view_proj[16] = {};
		bool view_proj_valid = false;
		bool view_proj_fresh = false;
		// Reconstructed camera eye of the current frame; only trusted after SetupCamera succeeded.
		float frame_eye[3] = {};
		bool frame_eye_valid = false;
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
				for (const auto& [hash, handle] : registered_meshes)
				{
					api_table.DestroyMesh(handle);
				}
			}

			registered_meshes.clear();
		}

		void destroy_registered_materials()
		{
			if (api_table.DestroyMaterial)
			{
				for (const auto& [hash, handle] : registered_materials)
				{
					api_table.DestroyMaterial(handle);
				}
			}

			registered_materials.clear();
		}

		api::remixapi_MaterialHandle find_material(u64 hash) const
		{
			const auto it = registered_materials.find(hash);
			return it == registered_materials.end() ? nullptr : it->second;
		}

		api::remixapi_MeshHandle find_mesh(u64 hash) const
		{
			const auto it = registered_meshes.find(hash);
			return it == registered_meshes.end() ? nullptr : it->second;
		}

		api::remixapi_MaterialHandle get_or_create_material(const mesh_capture& mesh)
		{
			if (!api_table.CreateMaterial || !api_table.DestroyMaterial)
			{
				return nullptr;
			}

			u64 hash = mesh.material_hash ? mesh.material_hash : 0x52504353334d4154ull;
			if (const auto existing = find_material(hash))
			{
				return existing;
			}

			if (registered_materials.size() >= cfg_max_registered_materials())
			{
				if (!logged_material_registry_full)
				{
					rsx_log.warning("RTX Remix: RSX material registry limit reached; using null materials for new meshes.");
					logged_material_registry_full = true;
				}

				return nullptr;
			}

			const bool has_texture = !mesh.albedo_texture_path.empty();

			api::remixapi_MaterialInfoOpaqueEXT opaque{};
			opaque.sType = api::REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
			// When a texture is bound, keep the albedo constant white so the captured
			// texture is shown faithfully instead of being tinted by the averaged vertex color.
			opaque.albedoConstant = has_texture
				? api::remixapi_Float3D{ 1.0f, 1.0f, 1.0f }
				: api::remixapi_Float3D{ mesh.albedo[0], mesh.albedo[1], mesh.albedo[2] };
			opaque.opacityConstant = std::clamp(mesh.opacity, 0.0f, 1.0f);
			opaque.roughnessConstant = 0.55f;
			opaque.metallicConstant = 0.0f;
			opaque.thinFilmThickness_value = 200.0f;
			opaque.useDrawCallAlphaState = true;
			opaque.alphaTestType = 7;

			// Keep the wide path alive until CreateMaterial has consumed it.
			[[maybe_unused]] std::wstring albedo_texture_wpath;

			api::remixapi_MaterialInfo material_info{};
			material_info.sType = api::REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
			material_info.pNext = &opaque;
			material_info.hash = hash;
			material_info.emissiveIntensity = 0.0f;
			material_info.emissiveColorConstant = { 0.0f, 0.0f, 0.0f };
			material_info.spriteSheetRow = 1;
			material_info.spriteSheetCol = 1;
			material_info.filterMode = 1;
			material_info.wrapModeU = 1;
			material_info.wrapModeV = 1;

#ifdef _WIN32
			if (has_texture)
			{
				albedo_texture_wpath = utf8_to_wchar(mesh.albedo_texture_path);
				material_info.albedoTexture = albedo_texture_wpath.c_str();
			}
#endif

			api::remixapi_MaterialHandle handle = nullptr;
			if (const auto status = api_table.CreateMaterial(&material_info, &handle);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: CreateMaterial for RSX draw failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
				return nullptr;
			}

			registered_materials.emplace(hash, handle);

			if (!logged_material_bridge)
			{
				rsx_log.notice("RTX Remix: RSX material bridge is creating opaque materials from vertex color state.");
				logged_material_bridge = true;
			}

			return handle;
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

		// Rebuild the game's real camera from the captured world->clip view-projection: invert it,
		// unproject the 8 clip-volume corners to world space, converge the near->far edge rays on
		// the eye and derive the parameterized camera from the near-plane rectangle. Any degenerate
		// or implausible result returns false so the caller falls back to the auto-fit camera.
		bool setup_camera_from_view_proj()
		{
			float inv_vp[16];
			if (!invert_mat4(view_proj, inv_vp))
			{
				return false;
			}

			// Corner index bits: 1 = +x, 2 = +y, 4 = far. RSX clip z spans [0, 1].
			std::array<std::array<float, 3>, 8> corners{};
			for (u32 i = 0; i < 8; i++)
			{
				const float x = (i & 1) ? 1.0f : -1.0f;
				const float y = (i & 2) ? 1.0f : -1.0f;
				const float z = (i & 4) ? 1.0f : 0.0f;

				if (!unproject_ndc(inv_vp, x, y, z, corners[i]))
				{
					return false;
				}
			}

			std::array<std::array<float, 3>, 4> origins{};
			std::array<std::array<float, 3>, 4> dirs{};
			for (u32 i = 0; i < 4; i++)
			{
				origins[i] = corners[i];
				dirs[i] = v3_sub(corners[i + 4], corners[i]);
			}

			std::array<float, 3> eye{};
			if (!converge_rays(origins, dirs, eye) ||
				std::abs(eye[0]) > max_remix_abs_position ||
				std::abs(eye[1]) > max_remix_abs_position ||
				std::abs(eye[2]) > max_remix_abs_position)
			{
				return false;
			}

			const auto average4 = [](const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c, const std::array<float, 3>& d)
			{
				return std::array<float, 3>
				{
					(a[0] + b[0] + c[0] + d[0]) * 0.25f,
					(a[1] + b[1] + c[1] + d[1]) * 0.25f,
					(a[2] + b[2] + c[2] + d[2]) * 0.25f
				};
			};

			const auto near_center = average4(corners[0], corners[1], corners[2], corners[3]);
			const auto far_center = average4(corners[4], corners[5], corners[6], corners[7]);

			std::array<float, 3> forward = v3_sub(near_center, eye);
			const float near_distance = v3_length(forward);
			if (!std::isfinite(near_distance) || near_distance < 1e-5f || !v3_normalize(forward))
			{
				return false;
			}

			const float far_distance = v3_dot(v3_sub(far_center, eye), forward);
			if (!std::isfinite(far_distance) || far_distance <= near_distance * 1.001f)
			{
				return false;
			}

			// Near-plane axes from the corner rectangle: +x edge midpoint minus -x, +y minus -y.
			std::array<float, 3> right_axis =
			{
				(corners[1][0] + corners[3][0] - corners[0][0] - corners[2][0]) * 0.5f,
				(corners[1][1] + corners[3][1] - corners[0][1] - corners[2][1]) * 0.5f,
				(corners[1][2] + corners[3][2] - corners[0][2] - corners[2][2]) * 0.5f
			};
			std::array<float, 3> up_axis =
			{
				(corners[2][0] + corners[3][0] - corners[0][0] - corners[1][0]) * 0.5f,
				(corners[2][1] + corners[3][1] - corners[0][1] - corners[1][1]) * 0.5f,
				(corners[2][2] + corners[3][2] - corners[0][2] - corners[1][2]) * 0.5f
			};

			const float half_width = v3_length(right_axis) * 0.5f;
			const float half_height = v3_length(up_axis) * 0.5f;
			if (!(half_width > 1e-6f) || !(half_height > 1e-6f))
			{
				return false;
			}

			const float fov_y_degrees = 2.0f * std::atan(half_height / near_distance) * 57.2957795f;
			const float aspect = half_width / half_height;
			if (!(fov_y_degrees > 1.0f) || !(fov_y_degrees < 175.0f) || !(aspect > 0.05f) || !(aspect < 20.0f))
			{
				return false;
			}

			// Gram-Schmidt against forward keeps the corner-derived orientation while giving Remix
			// a clean orthonormal basis.
			const float right_dot = v3_dot(right_axis, forward);
			right_axis = { right_axis[0] - forward[0] * right_dot, right_axis[1] - forward[1] * right_dot, right_axis[2] - forward[2] * right_dot };
			if (!v3_normalize(right_axis))
			{
				return false;
			}

			const float up_dot_forward = v3_dot(up_axis, forward);
			const float up_dot_right = v3_dot(up_axis, right_axis);
			up_axis =
			{
				up_axis[0] - forward[0] * up_dot_forward - right_axis[0] * up_dot_right,
				up_axis[1] - forward[1] * up_dot_forward - right_axis[1] * up_dot_right,
				up_axis[2] - forward[2] * up_dot_forward - right_axis[2] * up_dot_right
			};
			if (!v3_normalize(up_axis))
			{
				return false;
			}

			// The clip-space y sign is ambiguous (RSX/Vulkan y-down vs GL y-up conventions); this
			// knob flips the image the right way up when a game resolves it the other way.
			if (g_cfg.video.rtx_remix.camera_flip_up.get())
			{
				up_axis = { -up_axis[0], -up_axis[1], -up_axis[2] };
			}

			api::remixapi_CameraInfoParameterizedEXT camera_params{};
			camera_params.sType = api::REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
			camera_params.position = { eye[0], eye[1], eye[2] };
			camera_params.forward = { forward[0], forward[1], forward[2] };
			camera_params.up = { up_axis[0], up_axis[1], up_axis[2] };
			camera_params.right = { right_axis[0], right_axis[1], right_axis[2] };
			camera_params.fovYInDegrees = fov_y_degrees;
			camera_params.aspect = aspect;
			camera_params.nearPlane = std::max(0.001f, near_distance);
			camera_params.farPlane = std::max(far_distance, near_distance * 10.0f);

			api::remixapi_CameraInfo camera_info{};
			camera_info.sType = api::REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
			camera_info.pNext = &camera_params;
			camera_info.type = api::REMIXAPI_CAMERA_TYPE_WORLD;

			if (const auto status = api_table.SetupCamera(&camera_info);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: SetupCamera (game camera) failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
				return false;
			}

			// Commit the eye only after SetupCamera accepted it, so a rejected camera can never
			// leave the headlight at a phantom position.
			frame_eye[0] = eye[0];
			frame_eye[1] = eye[1];
			frame_eye[2] = eye[2];
			frame_eye_valid = true;
			return true;
		}

		void update_camera(u32 width, u32 height, bool auto_fit)
		{
			if (g_cfg.video.rtx_remix.use_game_camera.get() && view_proj_valid && setup_camera_from_view_proj())
			{
				if (!logged_game_camera)
				{
					rsx_log.success("RTX Remix: using the game camera reconstructed from vertex-program constants.");
					logged_game_camera = true;
				}

				return;
			}

			// Auto-fit fallback: no trusted eye this frame, so the headlight must not use a stale one.
			frame_eye_valid = false;
			setup_camera(width, height, auto_fit);
		}

		void submit_frame_light()
		{
			if (!api_table.CreateLight || !api_table.DestroyLight || !api_table.DrawLightInstance)
			{
				return;
			}

			// The scene light follows the camera and the scene, so it is recreated every frame.
			if (frame_light)
			{
				api_table.DestroyLight(frame_light);
				frame_light = nullptr;
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

			api::remixapi_LightInfo light_info{};
			light_info.sType = api::REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			light_info.pNext = &sphere_light;
			light_info.hash = 0x5250435333524c32ull;

			// The emitter must stay SMALL relative to the scene: any geometry inside the emitting
			// sphere is lit near-uniformly from every direction and reads as self-glowing. A
			// scene-scaled radius (previously up to thousands of units) swallowed most of the
			// world and made everything look emissive. Remix auto-exposure (on by default) sets
			// the overall image level, so a small emitter with fixed radiance is enough; the
			// intensity knob remains for per-game taste.
			const float emitter_radius = std::clamp(radius * 0.002f, 0.05f, 10.0f);
			const float radiance = 6000.0f * (static_cast<float>(g_cfg.video.rtx_remix.headlight_intensity.get()) / 100.0f);

			if (g_cfg.video.rtx_remix.headlight.get() && frame_eye_valid)
			{
				// Headlight at the reconstructed camera eye: whatever the camera sees is lit.
				sphere_light.position = { frame_eye[0], frame_eye[1], frame_eye[2] };
				sphere_light.radius = emitter_radius;
				light_info.radiance = { radiance, radiance, radiance };
			}
			else if (frame_bounds_valid)
			{
				// Legacy bounds-derived light for frames without a reconstructed eye.
				sphere_light.position = { center[0], center[1] - radius, center[2] - radius * 1.5f };
				sphere_light.radius = emitter_radius;
				light_info.radiance = { radiance, radiance, radiance };
			}
			else
			{
				return;
			}

			if (const auto status = api_table.CreateLight(&light_info, &frame_light);
				status != api::REMIXAPI_ERROR_CODE_SUCCESS)
			{
				rsx_log.warning("RTX Remix: CreateLight failed: %s (%u).", remix_error_to_string(status), static_cast<u32>(status));
				return;
			}

			api_table.DrawLightInstance(frame_light);
		}

		void reset_runtime()
		{
#ifdef _WIN32
			destroy_present_window();
			using_external_swapchain = false;

			particle_system.close();

			for (const auto cookie : dll_directory_cookies)
			{
				RemoveDllDirectory(cookie);
			}
			dll_directory_cookies.clear();
#endif
			destroy_registered_meshes();
			destroy_registered_materials();
			api_table = {};
			debug_mesh = nullptr;
			debug_light = nullptr;
			frame_light = nullptr;
			initialized = false;
			frame_active = false;
			scene_created = false;
			view_proj_valid = false;
			view_proj_fresh = false;
			frame_eye_valid = false;
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
		void destroy_present_window()
		{
			if (present_window)
			{
				DestroyWindow(present_window);
				present_window = nullptr;
			}
		}

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
			m_impl->destroy_present_window();
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

		// Reuse the previous frame's view-projection/bounds before collecting the new frame.
		// Remix command ordering makes this more reliable than setting the camera
		// only after all current-frame instances have already been submitted.
		m_impl->update_camera(width, height, m_impl->frame_bounds_valid);
		m_impl->reset_frame_bounds();
		m_impl->view_proj_fresh = false;
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

		// Record the frame's camera matrix before any mesh filtering: even a draw later dropped
		// by budget or sanity checks can donate a valid view-projection. First capture wins, since
		// games draw the main scene before overlays.
		if (mesh.has_view_proj && !m_impl->view_proj_fresh)
		{
			std::copy_n(mesh.view_proj, 16, m_impl->view_proj);
			m_impl->view_proj_valid = true;
			m_impl->view_proj_fresh = true;
		}

		if (mesh.vertices.size() < 3 || mesh.indices.size() < 3)
		{
			return;
		}

		if (m_impl->meshes_this_frame >= cfg_max_meshes_per_frame() ||
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

		api::remixapi_MeshHandle handle = m_impl->find_mesh(hash);

		if (!handle)
		{
			if (m_impl->registered_meshes.size() >= cfg_max_registered_meshes())
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
				dst.normal[0] = src.nx;
				dst.normal[1] = src.ny;
				dst.normal[2] = src.nz;
				dst.texcoord[0] = src.u;
				dst.texcoord[1] = src.v;
				dst.color = src.color;
				vertices.push_back(dst);
			}

			const auto material = m_impl->get_or_create_material(mesh);

			api::remixapi_MeshInfoSurfaceTriangles triangles{};
			triangles.vertices_values = vertices.data();
			triangles.vertices_count = vertices.size();
			triangles.indices_values = mesh.indices.data();
			triangles.indices_count = mesh.indices.size();
			triangles.material = material;

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
			m_impl->registered_meshes.emplace(hash, handle);
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
		m_impl->update_camera(width, height, m_impl->meshes_this_frame > 0);
		m_impl->submit_frame_light();

		if (!m_impl->logged_mesh_stats || (m_impl->frame_index && (m_impl->frame_index % 60) == 0))
		{
			rsx_log.notice("RTX Remix: frame stats: meshes=%u, registered=%zu, materials=%zu, rejected=%u, draws=%u.",
				m_impl->meshes_this_frame,
				m_impl->registered_meshes.size(),
				m_impl->registered_materials.size(),
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

#ifdef _WIN32
		m_impl->destroy_present_window();
#endif

		m_impl->destroy_registered_meshes();

		if (m_impl->debug_mesh && m_impl->api_table.DestroyMesh)
		{
			m_impl->api_table.DestroyMesh(m_impl->debug_mesh);
			m_impl->debug_mesh = nullptr;
		}

		m_impl->destroy_registered_materials();

		if (m_impl->debug_light && m_impl->api_table.DestroyLight)
		{
			m_impl->api_table.DestroyLight(m_impl->debug_light);
			m_impl->debug_light = nullptr;
		}

		if (m_impl->frame_light && m_impl->api_table.DestroyLight)
		{
			m_impl->api_table.DestroyLight(m_impl->frame_light);
			m_impl->frame_light = nullptr;
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
		reset_texture_cache();
		rsx_log.notice("RTX Remix: runtime stopped.");
	}
}
