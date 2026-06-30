#pragma once

#include <cstdint>

#if defined(_WIN32)
#define RPCS3_REMIXAPI_CALL __stdcall
#else
#define RPCS3_REMIXAPI_CALL
#endif

#define RPCS3_REMIXAPI_VERSION_MAKE(major, minor, patch) ( \
	(((std::uint64_t)(major)) << 48) | \
	(((std::uint64_t)(minor)) << 16) | \
	(((std::uint64_t)(patch))      ) )

#define RPCS3_REMIXAPI_VERSION_MAJOR 0
#define RPCS3_REMIXAPI_VERSION_MINOR 6
#define RPCS3_REMIXAPI_VERSION_PATCH 4

namespace rsx::remix::api
{
	struct HWND__;
	using remixapi_HWND = HWND__*;

	struct IDirect3D9Ex;
	struct IDirect3DDevice9Ex;
	struct IDirect3DSurface9;

	enum remixapi_StructType
	{
		REMIXAPI_STRUCT_TYPE_NONE = 0,
		REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO = 1,
		REMIXAPI_STRUCT_TYPE_MATERIAL_INFO = 2,
		REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT = 3,
		REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT = 4,
		REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT = 5,
		REMIXAPI_STRUCT_TYPE_LIGHT_INFO = 6,
		REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT = 7,
		REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT = 8,
		REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT = 9,
		REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT = 10,
		REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT = 11,
		REMIXAPI_STRUCT_TYPE_MESH_INFO = 12,
		REMIXAPI_STRUCT_TYPE_INSTANCE_INFO = 13,
		REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT = 14,
		REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT = 15,
		REMIXAPI_STRUCT_TYPE_CAMERA_INFO = 16,
		REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT = 17,
		REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT = 18,
		REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_OBJECT_PICKING_EXT = 19,
		REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT = 20,
		REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT = 21,
		REMIXAPI_STRUCT_TYPE_STARTUP_INFO = 22,
		REMIXAPI_STRUCT_TYPE_PRESENT_INFO = 23,
		REMIXAPI_STRUCT_TYPE_DEPRECATED_LEGACY_PARTICLE_SYSTEM = 24,
		REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_PARTICLE_SYSTEM_EXT = 25,
		REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT = 26,
		REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO = 27
	};

	enum remixapi_ErrorCode
	{
		REMIXAPI_ERROR_CODE_SUCCESS = 0,
		REMIXAPI_ERROR_CODE_GENERAL_FAILURE = 1,
		REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE = 2,
		REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS = 3,
		REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE = 4,
		REMIXAPI_ERROR_CODE_ALREADY_EXISTS = 5,
		REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D9_DEVICE = 6,
		REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED = 7,
		REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION = 8,
		REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE = 9,
		REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE = 10,
		REMIXAPI_ERROR_CODE_NOT_INITIALIZED = 11,
		REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES = 0x88960001,
		REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM = 0x88960002,
		REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL = 0x88960003,
		REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL = 0x88960004,
		REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL = 0x88960005,
		REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING = 0x88960006,
	};

	using remixapi_Bool = std::uint32_t;
	using remixapi_MaterialHandle = struct remixapi_MaterialHandle_T*;
	using remixapi_MeshHandle = struct remixapi_MeshHandle_T*;
	using remixapi_LightHandle = struct remixapi_LightHandle_T*;
	using remixapi_Path = const wchar_t*;

	struct remixapi_Float2D
	{
		float x;
		float y;
	};

	struct remixapi_Float3D
	{
		float x;
		float y;
		float z;
	};

	struct remixapi_Float4D
	{
		float x;
		float y;
		float z;
		float w;
	};

	struct remixapi_Rect2D
	{
		std::int32_t left;
		std::int32_t top;
		std::int32_t right;
		std::int32_t bottom;
	};

	struct remixapi_Transform
	{
		float matrix[3][4];
	};

	struct remixapi_StartupInfo
	{
		remixapi_StructType sType;
		void* pNext;
		remixapi_HWND hwnd;
		remixapi_Bool disableSrgbConversionForOutput;
		remixapi_Bool forceNoVkSwapchain;
		remixapi_Bool editorModeEnabled;
	};

	struct remixapi_PresentInfo
	{
		remixapi_StructType sType;
		void* pNext;
		remixapi_HWND hwndOverride;
	};

	struct remixapi_InitializeLibraryInfo
	{
		remixapi_StructType sType;
		void* pNext;
		std::uint64_t version;
	};

	struct remixapi_MaterialInfo;
	struct remixapi_CameraMediumInfo;

	struct remixapi_HardcodedVertex
	{
		float position[3];
		float normal[3];
		float texcoord[2];
		std::uint32_t color;
		std::uint32_t _pad0;
		std::uint32_t _pad1;
		std::uint32_t _pad2;
		std::uint32_t _pad3;
		std::uint32_t _pad4;
		std::uint32_t _pad5;
		std::uint32_t _pad6;
	};

	struct remixapi_MeshInfoSkinning
	{
		std::uint32_t bonesPerVertex;
		const float* blendWeights_values;
		std::uint32_t blendWeights_count;
		const std::uint32_t* blendIndices_values;
		std::uint32_t blendIndices_count;
	};

	struct remixapi_MeshInfoSurfaceTriangles
	{
		const remixapi_HardcodedVertex* vertices_values;
		std::uint64_t vertices_count;
		const std::uint32_t* indices_values;
		std::uint64_t indices_count;
		remixapi_Bool skinning_hasvalue;
		remixapi_MeshInfoSkinning skinning_value;
		remixapi_MaterialHandle material;
	};

	struct remixapi_MeshInfo
	{
		remixapi_StructType sType;
		void* pNext;
		std::uint64_t hash;
		const remixapi_MeshInfoSurfaceTriangles* surfaces_values;
		std::uint32_t surfaces_count;
	};

	enum remixapi_CameraType
	{
		REMIXAPI_CAMERA_TYPE_WORLD,
		REMIXAPI_CAMERA_TYPE_SKY,
		REMIXAPI_CAMERA_TYPE_VIEW_MODEL,
	};

	struct remixapi_CameraInfoParameterizedEXT
	{
		remixapi_StructType sType;
		void* pNext;
		remixapi_Float3D position;
		remixapi_Float3D forward;
		remixapi_Float3D up;
		remixapi_Float3D right;
		float fovYInDegrees;
		float aspect;
		float nearPlane;
		float farPlane;
	};

	struct remixapi_CameraInfo
	{
		remixapi_StructType sType;
		void* pNext;
		remixapi_CameraType type;
		float view[4][4];
		float projection[4][4];
	};

	using remixapi_InstanceCategoryFlags = std::uint32_t;

	struct remixapi_InstanceInfo
	{
		remixapi_StructType sType;
		void* pNext;
		remixapi_InstanceCategoryFlags categoryFlags;
		remixapi_MeshHandle mesh;
		remixapi_Transform transform;
		remixapi_Bool doubleSided;
	};

	struct remixapi_LightInfoLightShaping
	{
		remixapi_Float3D direction;
		float coneAngleDegrees;
		float coneSoftness;
		float focusExponent;
	};

	struct remixapi_LightInfoSphereEXT
	{
		remixapi_StructType sType;
		void* pNext;
		remixapi_Float3D position;
		float radius;
		remixapi_Bool shaping_hasvalue;
		remixapi_LightInfoLightShaping shaping_value;
		float volumetricRadianceScale;
	};

	struct remixapi_LightInfo
	{
		remixapi_StructType sType;
		void* pNext;
		std::uint64_t hash;
		remixapi_Float3D radiance;
	};

	using PFN_remixapi_Startup = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_StartupInfo* info);
	using PFN_remixapi_Shutdown = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(void);
	using PFN_remixapi_CreateMaterial = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out_handle);
	using PFN_remixapi_DestroyMaterial = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(remixapi_MaterialHandle handle);
	using PFN_remixapi_CreateMesh = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_MeshInfo* info, remixapi_MeshHandle* out_handle);
	using PFN_remixapi_DestroyMesh = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(remixapi_MeshHandle handle);
	using PFN_remixapi_SetupCamera = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_CameraInfo* info);
	using PFN_remixapi_DrawInstance = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_InstanceInfo* info);
	using PFN_remixapi_CreateLight = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_LightInfo* info, remixapi_LightHandle* out_handle);
	using PFN_remixapi_DestroyLight = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(remixapi_LightHandle handle);
	using PFN_remixapi_DrawLightInstance = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(remixapi_LightHandle lightHandle);
	using PFN_remixapi_SetConfigVariable = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const char* key, const char* value);
	using PFN_remixapi_Present = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_PresentInfo* info);

	using PFN_remixapi_dxvk_CreateD3D9 = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(remixapi_Bool editorModeEnabled, IDirect3D9Ex** out_pD3D9);
	using PFN_remixapi_dxvk_RegisterD3D9Device = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(IDirect3DDevice9Ex* d3d9Device);
	using PFN_remixapi_dxvk_GetExternalSwapchain = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(std::uint64_t* out_vkImage, std::uint64_t* out_vkSemaphoreRenderingDone, std::uint64_t* out_vkSemaphoreResumeSemaphore);
	using PFN_remixapi_dxvk_GetVkImage = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(IDirect3DSurface9* source, std::uint64_t* out_vkImage);
	using PFN_remixapi_dxvk_CopyRenderingOutput = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(IDirect3DSurface9* destination, int type);
	using PFN_remixapi_dxvk_SetDefaultOutput = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(int type, const remixapi_Float4D* color);
	using PFN_remixapi_pick_RequestObjectPicking = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_Rect2D* pixelRegion, void* callback, void* callbackUserData);
	using PFN_remixapi_pick_HighlightObjects = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const std::uint32_t* objectPickingValues_values, std::uint32_t objectPickingValues_count, std::uint8_t colorR, std::uint8_t colorG, std::uint8_t colorB);
	using PFN_remixapi_SetCameraMediumMaterial = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_CameraMediumInfo* info);

	struct remixapi_Interface
	{
		PFN_remixapi_Shutdown Shutdown;
		PFN_remixapi_CreateMaterial CreateMaterial;
		PFN_remixapi_DestroyMaterial DestroyMaterial;
		PFN_remixapi_CreateMesh CreateMesh;
		PFN_remixapi_DestroyMesh DestroyMesh;
		PFN_remixapi_SetupCamera SetupCamera;
		PFN_remixapi_DrawInstance DrawInstance;
		PFN_remixapi_CreateLight CreateLight;
		PFN_remixapi_DestroyLight DestroyLight;
		PFN_remixapi_DrawLightInstance DrawLightInstance;
		PFN_remixapi_SetConfigVariable SetConfigVariable;
		PFN_remixapi_dxvk_CreateD3D9 dxvk_CreateD3D9;
		PFN_remixapi_dxvk_RegisterD3D9Device dxvk_RegisterD3D9Device;
		PFN_remixapi_dxvk_GetExternalSwapchain dxvk_GetExternalSwapchain;
		PFN_remixapi_dxvk_GetVkImage dxvk_GetVkImage;
		PFN_remixapi_dxvk_CopyRenderingOutput dxvk_CopyRenderingOutput;
		PFN_remixapi_dxvk_SetDefaultOutput dxvk_SetDefaultOutput;
		PFN_remixapi_pick_RequestObjectPicking pick_RequestObjectPicking;
		PFN_remixapi_pick_HighlightObjects pick_HighlightObjects;
		PFN_remixapi_Startup Startup;
		PFN_remixapi_Present Present;
		PFN_remixapi_SetCameraMediumMaterial SetCameraMediumMaterial;
	};

	using PFN_remixapi_InitializeLibrary = remixapi_ErrorCode(RPCS3_REMIXAPI_CALL *)(const remixapi_InitializeLibraryInfo* info, remixapi_Interface* out_result);

	static_assert(sizeof(remixapi_HardcodedVertex) == 64);
	static_assert(sizeof(remixapi_Interface) == sizeof(void*) * 22);
}

#undef RPCS3_REMIXAPI_CALL
