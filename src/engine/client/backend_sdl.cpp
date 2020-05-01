#include <base/detect.h>

#include <base/tl/threading.h>

#include <base/tl/array.h>

#include "SDL.h"
#include "SDL_syswm.h"

#include "graphics_threaded.h"
#include "backend_sdl.h"

#include "shaders.h"

void FequestAdapterCallback(WGPUAdapterId Received, void *pUserdata)
{
	*(WGPUAdapterId*)pUserdata = Received;
}

// ------------ CGraphicsBackend_Threaded

void CGraphicsBackend_Threaded::ThreadFunc(void *pUser)
{
	CGraphicsBackend_Threaded *pThis = (CGraphicsBackend_Threaded *)pUser;

	while(!pThis->m_Shutdown)
	{
		pThis->m_Activity.wait();
		if(pThis->m_pBuffer)
		{
			#ifdef CONF_PLATFORM_MACOSX
				CAutoreleasePool AutoreleasePool;
			#endif
			pThis->m_pProcessor->RunBuffer(pThis->m_pBuffer);
			sync_barrier();
			pThis->m_pBuffer = 0x0;
			pThis->m_BufferDone.signal();
		}
	}
}

CGraphicsBackend_Threaded::CGraphicsBackend_Threaded()
{
	m_pBuffer = 0x0;
	m_pProcessor = 0x0;
	m_pThread = 0x0;
}

void CGraphicsBackend_Threaded::StartProcessor(ICommandProcessor *pProcessor)
{
	m_Shutdown = false;
	m_pProcessor = pProcessor;
	m_pThread = thread_init(ThreadFunc, this);
	m_BufferDone.signal();
}

void CGraphicsBackend_Threaded::StopProcessor()
{
	m_Shutdown = true;
	m_Activity.signal();
	thread_wait(m_pThread);
	thread_destroy(m_pThread);
}

void CGraphicsBackend_Threaded::RunBuffer(CCommandBuffer *pBuffer)
{
	WaitForIdle();
	m_pBuffer = pBuffer;
	m_Activity.signal();
}

bool CGraphicsBackend_Threaded::IsIdle() const
{
	return m_pBuffer == 0x0;
}

void CGraphicsBackend_Threaded::WaitForIdle()
{
	while(m_pBuffer != 0x0)
		m_BufferDone.wait();
}


// ------------ CCommandProcessorFragment_General

void CCommandProcessorFragment_General::Cmd_Signal(const CCommandBuffer::CSignalCommand *pCommand)
{
	pCommand->m_pSemaphore->signal();
}

bool CCommandProcessorFragment_General::RunCommand(const CCommandBuffer::CCommand * pBaseCommand)
{
	switch(pBaseCommand->m_Cmd)
	{
	case CCommandBuffer::CMD_NOP: break;
	case CCommandBuffer::CMD_SIGNAL: Cmd_Signal(static_cast<const CCommandBuffer::CSignalCommand *>(pBaseCommand)); break;
	default: return false;
	}

	return true;
}

// ------------ CCommandProcessorFragment_WGPU

unsigned char CCommandProcessorFragment_WGPU::Sample(int w, int h, const unsigned char *pData, int u, int v, int Offset, int ScaleW, int ScaleH, int Bpp)
{
	int Value = 0;
	for(int x = 0; x < ScaleW; x++)
		for(int y = 0; y < ScaleH; y++)
			Value += pData[((v+y)*w+(u+x))*Bpp+Offset];
	return Value/(ScaleW*ScaleH);
}

unsigned char *CCommandProcessorFragment_WGPU::Rescale(int Width, int Height, int NewWidth, int NewHeight, int Format, const unsigned char *pData)
{
	unsigned char *pTmpData;
	int ScaleW = Width/NewWidth;
	int ScaleH = Height/NewHeight;

	int Bpp = 3;
	if(Format == CCommandBuffer::TEXFORMAT_RGBA)
		Bpp = 4;

	pTmpData = (unsigned char *)mem_alloc(NewWidth*NewHeight*Bpp, 1);

	int c = 0;
	for(int y = 0; y < NewHeight; y++)
		for(int x = 0; x < NewWidth; x++, c++)
		{
			pTmpData[c*Bpp] = Sample(Width, Height, pData, x*ScaleW, y*ScaleH, 0, ScaleW, ScaleH, Bpp);
			pTmpData[c*Bpp+1] = Sample(Width, Height, pData, x*ScaleW, y*ScaleH, 1, ScaleW, ScaleH, Bpp);
			pTmpData[c*Bpp+2] = Sample(Width, Height, pData, x*ScaleW, y*ScaleH, 2, ScaleW, ScaleH, Bpp);
			if(Bpp == 4)
				pTmpData[c*Bpp+3] = Sample(Width, Height, pData, x*ScaleW, y*ScaleH, 3, ScaleW, ScaleH, Bpp);
		}

	return pTmpData;
}

void CCommandProcessorFragment_WGPU::ConvertToRGBA(int Width, int Height, int Format, unsigned char **ppData)
{
	unsigned char *pTexels = *ppData;

	if(Format == CCommandBuffer::TEXFORMAT_RGB)
	{
		unsigned char *pTmpData = (unsigned char *)mem_alloc(Width*Height*4, 1);
		for(int i = 0; i < Width * Height; ++i)
		{
			pTmpData[i*4+0] = pTexels[i*3+0];
			pTmpData[i*4+1] = pTexels[i*3+1];
			pTmpData[i*4+2] = pTexels[i*3+2];
			pTmpData[i*4+3] = 255;
		}
		mem_free(pTexels);
		*ppData = pTmpData;
	}
	else if(Format == CCommandBuffer::TEXFORMAT_ALPHA)
	{
		unsigned char *pTmpData = (unsigned char *)mem_alloc(Width*Height*4, 1);
		for(int i = 0; i < Width * Height; ++i)
		{
			pTmpData[i*4+0] = pTexels[i];
			pTmpData[i*4+1] = pTexels[i];
			pTmpData[i*4+2] = pTexels[i];
			pTmpData[i*4+3] = pTexels[i];
		}
		mem_free(pTexels);
		*ppData = pTmpData;
	}
	// use premultiplied alpha for rgba textures
	else if(Format == CCommandBuffer::TEXFORMAT_RGBA)
	{	
		for(int i = 0; i < Width * Height; ++i)
		{
			const float a = (pTexels[i*4+3]/255.0f);
			pTexels[i*4+0] = (unsigned char)(pTexels[i*4+0] * a);
			pTexels[i*4+1] = (unsigned char)(pTexels[i*4+1] * a);
			pTexels[i*4+2] = (unsigned char)(pTexels[i*4+2] * a);
		}
	}
}

void CCommandProcessorFragment_WGPU::SetState(const CCommandBuffer::CState &State, int PrimType, WGPURenderPassId RPass)
{
	if(State.m_ClipEnable)
	{
		int ClipY = m_ScreenHeight - State.m_ClipH - State.m_ClipY;
		wgpu_render_pass_set_scissor_rect(RPass, State.m_ClipX, ClipY, State.m_ClipW, State.m_ClipH);
	}
	else
	{
		wgpu_render_pass_set_scissor_rect(RPass, 0, 0, m_ScreenWidth, m_ScreenHeight);
	}

	WGPURenderPipelineId *pPipeline = m_RenderPipeline;
	CTextureData *pTex = 0;
	bool PMA = true;
	bool TexArray = false;

	if(PrimType == CCommandBuffer::PRIMTYPE_LINES)
	{
		pPipeline = m_RenderPipelineLines;
	}
	else
	{
		if(State.m_Texture >= 0 && State.m_Texture < CCommandBuffer::MAX_TEXTURES)
		{
			if(State.m_Dimension == 2 && (m_aTextures[State.m_Texture].m_State&CTexture::STATE_TEX2D))
			{
				pPipeline = m_Render2DPipeline;
				pTex = &m_aTextures[State.m_Texture].m_Tex2D;
			}
			else if(State.m_Dimension == 3 && (m_aTextures[State.m_Texture].m_State&CTexture::STATE_TEX3D))
			{
				pPipeline = m_Render2DArrayPipeline;
				pTex = &m_aTextures[State.m_Texture].m_Tex3D;
				TexArray = true;
			}
			else
				dbg_msg("render", "invalid texture %d %d %d\n", State.m_Texture, State.m_Dimension, m_aTextures[State.m_Texture].m_State);

			if(m_aTextures[State.m_Texture].m_Format != CCommandBuffer::TEXFORMAT_RGBA)
				PMA = false;
		}
	}

	int BlendMode = 0;

	// TODO: add additive blend mode?
	switch(State.m_BlendMode)
	{
	case CCommandBuffer::BLEND_NONE:
		BlendMode = 0;
		break;
	case CCommandBuffer::BLEND_ALPHA:
		BlendMode = PMA ? 2 : 1;
		break;
	default:
		dbg_msg("render", "unknown blendmode %d\n", State.m_BlendMode);
	};

	wgpu_render_pass_set_pipeline(RPass, pPipeline[BlendMode]);
	if(pTex)
	{
		WGPUBindGroupId TexBindGroup = TexArray
			? GetTexBindGroup(pTex, IGraphics::WRAP_CLAMP, IGraphics::WRAP_CLAMP, true)
			: GetTexBindGroup(pTex, State.m_WrapModeU, State.m_WrapModeV, false);
		wgpu_render_pass_set_bind_group(RPass, 1, TexBindGroup, NULL, 0);
	}

	CScreen CurScreen = {
		State.m_ScreenTL.x,
		State.m_ScreenTL.y,
		State.m_ScreenBR.x,
		State.m_ScreenBR.y
	};
	if(m_ScreenCount == 0 || mem_comp(&m_LastScreen, &CurScreen, sizeof(CScreen)) != 0)
	{
		mem_copy(&m_LastScreen, &CurScreen, sizeof(CScreen));
		m_ScreenCount++;
	}

	unsigned DynAlignment = (sizeof(CMat4) + WGPUBIND_BUFFER_ALIGNMENT - 1) / WGPUBIND_BUFFER_ALIGNMENT * WGPUBIND_BUFFER_ALIGNMENT;
	unsigned Offset = DynAlignment * (m_ScreenCount - 1);
	wgpu_render_pass_set_bind_group(RPass, 0, m_TransformBindGroup, &Offset, 1);
}

void CCommandProcessorFragment_WGPU::Cmd_Init(const CInitCommand *pCommand)
{
	// set some default settings
	/*
	glAlphaFunc(GL_GREATER, 0);
	glEnable(GL_ALPHA_TEST);
	glDepthMask(0);

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_MaxTexSize);
	glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &m_Max3DTexSize);
	dbg_msg("render", "opengl max texture sizes: %d, %d(3D)", m_MaxTexSize, m_Max3DTexSize);
	*/

	m_Device = pCommand->m_Device;
	m_ScreenWidth = pCommand->m_ScreenWidth;
	m_ScreenHeight = pCommand->m_ScreenHeight;
	m_pTextureMemoryUsage = pCommand->m_pTextureMemoryUsage;
	*m_pTextureMemoryUsage = 0;

	m_SwapChain = wgpu_device_create_swap_chain(m_Device, pCommand->m_Surface,
		&WGPUSwapChainDescriptor {
			.usage = WGPUTextureUsage_OUTPUT_ATTACHMENT,
			.format = WGPUTextureFormat_Bgra8Unorm,
			.width = m_ScreenWidth,
			.height = m_ScreenHeight,
			.present_mode = WGPUPresentMode_Immediate,
		});
	
	dbg_msg("wgpu", "created swapchain: %llu", m_SwapChain);

	WGPUShaderModuleId VertexShader = wgpu_device_create_shader_module(m_Device,
		&WGPUShaderModuleDescriptor {
			.code = WGPUU32Array {
				.bytes = (uint32_t*) s_aVert,
				.length = sizeof(s_aVert) / 4,
			},
		});

	dbg_msg("wgpu", "created vertex shader: %llu", VertexShader);

	WGPUShaderModuleId FragmentShader = wgpu_device_create_shader_module(m_Device,
		&WGPUShaderModuleDescriptor {
			.code = WGPUU32Array {
				.bytes = (uint32_t*) s_aFragNoTex,
				.length = sizeof(s_aFragNoTex) / 4,
			},
		});

	WGPUShaderModuleId FragmentShader2D = wgpu_device_create_shader_module(m_Device,
		&WGPUShaderModuleDescriptor {
			.code = WGPUU32Array {
				.bytes = (uint32_t*) s_aFrag2D,
				.length = sizeof(s_aFrag2D) / 4,
			},
		});

	WGPUShaderModuleId FragmentShader2DArray = wgpu_device_create_shader_module(m_Device,
		&WGPUShaderModuleDescriptor {
			.code = WGPUU32Array {
				.bytes = (uint32_t*) s_aFrag2DArray,
				.length = sizeof(s_aFrag2DArray) / 4,
			},
		});

	dbg_msg("wgpu", "created fragment shader: %llu, %llu, %llu", FragmentShader, FragmentShader2D, FragmentShader2DArray);

	WGPUBindGroupLayoutEntry aEntriesTransform[1] = {
		{
			.binding = 0,
			.visibility = WGPUShaderStage_VERTEX,
			.ty = WGPUBindingType_UniformBuffer,
			.has_dynamic_offset = true
		}
	};

	WGPUBindGroupLayoutEntry aEntries2D[2] = {
		{
			.binding = 0,
			.visibility = WGPUShaderStage_FRAGMENT,
			.ty = WGPUBindingType_SampledTexture,
			.multisampled = false,
			.view_dimension = WGPUTextureViewDimension_D2,
			.texture_component_type = WGPUTextureComponentType_Float
		},
		{
			.binding = 1,
			.visibility = WGPUShaderStage_FRAGMENT,
			.ty = WGPUBindingType_Sampler,
		}
	};

	WGPUBindGroupLayoutEntry aEntries2DArray[2] = {
		{
			.binding = 0,
			.visibility = WGPUShaderStage_FRAGMENT,
			.ty = WGPUBindingType_SampledTexture,
			.multisampled = false,
			.view_dimension = WGPUTextureViewDimension_D2Array,
			.texture_component_type = WGPUTextureComponentType_Float
		},
		{
			.binding = 1,
			.visibility = WGPUShaderStage_FRAGMENT,
			.ty = WGPUBindingType_Sampler,
		}
	};

	m_BindGroupTransformLayout = wgpu_device_create_bind_group_layout(m_Device,
		&WGPUBindGroupLayoutDescriptor {
			.label = "bind group transform layout",
			.entries = aEntriesTransform,
			.entries_length = 1,
		});

	m_BindGroup2DLayout = wgpu_device_create_bind_group_layout(m_Device,
		&WGPUBindGroupLayoutDescriptor {
			.label = "bind group 2D layout",
			.entries = aEntries2D,
			.entries_length = 2,
		});

	m_BindGroup2DArrayLayout = wgpu_device_create_bind_group_layout(m_Device,
		&WGPUBindGroupLayoutDescriptor {
			.label = "bind group 2D array layout",
			.entries = aEntries2DArray,
			.entries_length = 2,
		});

	dbg_msg("wgpu", "created bind group layout: %llu, %llu, %llu", m_BindGroupTransformLayout, m_BindGroup2DLayout, m_BindGroup2DArrayLayout);

	WGPUPipelineLayoutId PipelineLayout = wgpu_device_create_pipeline_layout(m_Device,
		&WGPUPipelineLayoutDescriptor {
			.bind_group_layouts = &m_BindGroupTransformLayout,
			.bind_group_layouts_length = 1,
		});

	WGPUBindGroupLayoutId aBindGroup2DLayouts[2] = {m_BindGroupTransformLayout, m_BindGroup2DLayout};
	WGPUPipelineLayoutId Pipeline2DLayout = wgpu_device_create_pipeline_layout(m_Device,
		&WGPUPipelineLayoutDescriptor {
			.bind_group_layouts = aBindGroup2DLayouts,
			.bind_group_layouts_length = 2,
		});

	WGPUBindGroupLayoutId aBindGroup2DArrayLayouts[2] = {m_BindGroupTransformLayout, m_BindGroup2DArrayLayout};
	WGPUPipelineLayoutId Pipeline2DArrayLayout = wgpu_device_create_pipeline_layout(m_Device,
		&WGPUPipelineLayoutDescriptor {
			.bind_group_layouts = aBindGroup2DArrayLayouts,
			.bind_group_layouts_length = 2,
		});

	dbg_msg("wgpu", "created pipeline layout: %llu, %llu, %llu", PipelineLayout, Pipeline2DLayout, Pipeline2DArrayLayout);

	WGPUBlendDescriptor BlendNone = {
		.src_factor = WGPUBlendFactor_One,
		.dst_factor = WGPUBlendFactor_Zero,
		.operation = WGPUBlendOperation_Add,
	};

	WGPUBlendDescriptor BlendNormal = {
		.src_factor = WGPUBlendFactor_SrcAlpha,
		.dst_factor = WGPUBlendFactor_OneMinusSrcAlpha,
		.operation = WGPUBlendOperation_Add,
	};

	WGPUBlendDescriptor BlendNormalPMA = {
		.src_factor = WGPUBlendFactor_One,
		.dst_factor = WGPUBlendFactor_OneMinusSrcAlpha,
		.operation = WGPUBlendOperation_Add,
	};

	m_RenderPipeline[0] = CreateRenderPipeline(PipelineLayout, VertexShader, FragmentShader, CCommandBuffer::PRIMTYPE_TRIANGLES, BlendNone);
	m_RenderPipeline[1] = 0;
	m_RenderPipeline[2] = CreateRenderPipeline(PipelineLayout, VertexShader, FragmentShader, CCommandBuffer::PRIMTYPE_TRIANGLES, BlendNormalPMA);

	m_Render2DPipeline[0] = CreateRenderPipeline(Pipeline2DLayout, VertexShader, FragmentShader2D, CCommandBuffer::PRIMTYPE_TRIANGLES, BlendNone);
	m_Render2DPipeline[1] = CreateRenderPipeline(Pipeline2DLayout, VertexShader, FragmentShader2D, CCommandBuffer::PRIMTYPE_TRIANGLES, BlendNormal);
	m_Render2DPipeline[2] = CreateRenderPipeline(Pipeline2DLayout, VertexShader, FragmentShader2D, CCommandBuffer::PRIMTYPE_TRIANGLES, BlendNormalPMA);

	m_Render2DArrayPipeline[0] = CreateRenderPipeline(Pipeline2DArrayLayout, VertexShader, FragmentShader2DArray, CCommandBuffer::PRIMTYPE_TRIANGLES, BlendNone);
	m_Render2DArrayPipeline[1] = CreateRenderPipeline(Pipeline2DArrayLayout, VertexShader, FragmentShader2DArray, CCommandBuffer::PRIMTYPE_TRIANGLES, BlendNormal);
	m_Render2DArrayPipeline[2] = CreateRenderPipeline(Pipeline2DArrayLayout, VertexShader, FragmentShader2DArray, CCommandBuffer::PRIMTYPE_TRIANGLES, BlendNormalPMA);

	m_RenderPipelineLines[0] = CreateRenderPipeline(PipelineLayout, VertexShader, FragmentShader, CCommandBuffer::PRIMTYPE_LINES, BlendNone);
	m_RenderPipelineLines[1] = 0;
	m_RenderPipelineLines[2] = CreateRenderPipeline(PipelineLayout, VertexShader, FragmentShader, CCommandBuffer::PRIMTYPE_LINES, BlendNormalPMA);

	dbg_msg("wgpu", "created pipelines");

	m_Sampler[0] = CreateSampler(IGraphics::WRAP_REPEAT, IGraphics::WRAP_REPEAT);
	m_Sampler[1] = CreateSampler(IGraphics::WRAP_CLAMP, IGraphics::WRAP_REPEAT);
	m_Sampler[2] = CreateSampler(IGraphics::WRAP_REPEAT, IGraphics::WRAP_CLAMP);
	m_Sampler[3] = CreateSampler(IGraphics::WRAP_CLAMP, IGraphics::WRAP_CLAMP);

	dbg_msg("wgpu", "created samplers");

	m_NextTexture = wgpu_swap_chain_get_next_texture(m_SwapChain);

	m_Ready = true;
}

void CCommandProcessorFragment_WGPU::Cmd_Texture_Update(const CCommandBuffer::CTextureUpdateCommand *pCommand)
{
	int Width = pCommand->m_Width;
	int Height = pCommand->m_Height;
	int Format = pCommand->m_Format;
	unsigned char *pTexData = (unsigned char*)pCommand->m_pData;

	ConvertToRGBA(Width, Height, Format, &pTexData);

	CTexture *pTex = &m_aTextures[pCommand->m_Slot];

	if(pTex->m_State&CTexture::STATE_TEX2D)
	{	
		unsigned Bpp = 4;
		unsigned MemSize = Width * Height * Bpp;

		uint8_t *pStagingMem;
		WGPUBufferId TmpBuffer = wgpu_device_create_buffer_mapped(m_Device,
			&WGPUBufferDescriptor {
				.size = MemSize,
				.usage = WGPUBufferUsage_COPY_SRC},
			&pStagingMem);

		mem_copy(pStagingMem, pTexData, MemSize);
		wgpu_buffer_unmap(TmpBuffer);

		//dbg_msg("wgpu", "created buffer: %llu", TmpBuffer);

		WGPUCommandEncoderId CmdEncoder = GetCommandEncoder();

		WGPUExtent3d TexExtent = {
			.width = (unsigned)Width,
			.height = (unsigned)Height,
			.depth = 1,
		};

		wgpu_command_encoder_copy_buffer_to_texture(CmdEncoder, 
			&WGPUBufferCopyView {
				.buffer = TmpBuffer,
				.offset = 0,
				.bytes_per_row = Width * Bpp,
				.rows_per_image = 0
			},
			&WGPUTextureCopyView {
				.texture = pTex->m_Tex2D.m_Tex,
				.mip_level = 0,
				.array_layer = 0,
				.origin = { (unsigned)pCommand->m_X, (unsigned)pCommand->m_Y, 0 }
			},
			TexExtent);

		wgpu_buffer_destroy(TmpBuffer);
		
		SubmitCommandBuffer();
		//dbg_msg("wgpu", "submitted copy command (update)");
	}
	mem_free(pTexData);
}

void CCommandProcessorFragment_WGPU::Cmd_Texture_Destroy(const CCommandBuffer::CTextureDestroyCommand *pCommand)
{
	EndRenderPass();
	CTexture *pTex = &m_aTextures[pCommand->m_Slot];
	if(pTex->m_State&CTexture::STATE_TEX2D)
	{
		for(int i = 0; i < 4; i++)
			if(pTex->m_Tex2D.m_BindGroups[i])
				wgpu_bind_group_destroy(pTex->m_Tex2D.m_BindGroups[i]);
		wgpu_texture_view_destroy(pTex->m_Tex2D.m_TexView);
		wgpu_texture_destroy(pTex->m_Tex2D.m_Tex);
	}
	if(pTex->m_State&CTexture::STATE_TEX3D)
	{
		for(int i = 0; i < 4; i++)
			if(pTex->m_Tex3D.m_BindGroups[i])
				wgpu_bind_group_destroy(pTex->m_Tex3D.m_BindGroups[i]);
		wgpu_texture_view_destroy(pTex->m_Tex3D.m_TexView);
		wgpu_texture_destroy(pTex->m_Tex3D.m_Tex);
	}
	*m_pTextureMemoryUsage -= pTex->m_MemSize;
	pTex->m_State = CTexture::STATE_EMPTY;
	pTex->m_MemSize = 0;
}

void CCommandProcessorFragment_WGPU::Cmd_Texture_Create(const CCommandBuffer::CTextureCreateCommand *pCommand)
{
	int Width = pCommand->m_Width;
	int Height = pCommand->m_Height;
	int Depth = 1;
	int Format = pCommand->m_Format;
	unsigned char *pTexData = (unsigned char*)pCommand->m_pData;

	// TODO: mipmaps, check limits, optimize allocations and conversions

	// resample if needed
	if(Format == CCommandBuffer::TEXFORMAT_RGBA || Format == CCommandBuffer::TEXFORMAT_RGB)
	{
		/*
		int MaxTexSize = m_MaxTexSize;
		if(pCommand->m_Flags&CCommandBuffer::TEXFLAG_TEXTURE3D)
		{
			if(pCommand->m_Flags&CCommandBuffer::TEXFLAG_TEXTURE2D)
				MaxTexSize = min(MaxTexSize, m_Max3DTexSize * IGraphics::NUMTILES_DIMENSION);
			else
				MaxTexSize = m_Max3DTexSize * IGraphics::NUMTILES_DIMENSION;
		}
		*/

		int MaxTexSize = 16 * 1024;
		if(Width > MaxTexSize || Height > MaxTexSize)
		{
			do
			{
				Width>>=1;
				Height>>=1;
			}
			while(Width > MaxTexSize || Height > MaxTexSize);

			unsigned char *pTmpData = Rescale(pCommand->m_Width, pCommand->m_Height, Width, Height, Format, pTexData);
			mem_free(pTexData);
			pTexData = pTmpData;
		}
		else if(Width > IGraphics::NUMTILES_DIMENSION && Height > IGraphics::NUMTILES_DIMENSION && (pCommand->m_Flags&CCommandBuffer::TEXFLAG_QUALITY) == 0)
		{
			Width>>=1;
			Height>>=1;

			unsigned char *pTmpData = Rescale(pCommand->m_Width, pCommand->m_Height, Width, Height, Format, pTexData);
			mem_free(pTexData);
			pTexData = pTmpData;
		}
	}

	ConvertToRGBA(Width, Height, Format, &pTexData);

	//
	m_aTextures[pCommand->m_Slot].m_Format = Format;

	// 2D texture
	if(pCommand->m_Flags&CCommandBuffer::TEXFLAG_TEXTURE2D)
	{
		bool Mipmaps = false; //!(pCommand->m_Flags&CCommandBuffer::TEXFLAG_NOMIPMAPS);
		m_aTextures[pCommand->m_Slot].m_State |= CTexture::STATE_TEX2D;
		CTextureData *pTex = &m_aTextures[pCommand->m_Slot].m_Tex2D;

		WGPUExtent3d TexExtent = {
			.width = (unsigned)Width,
			.height = (unsigned)Height,
			.depth = (unsigned)Depth,
		};
		pTex->m_Tex = wgpu_device_create_texture(m_Device, 
			&WGPUTextureDescriptor {
				.size = TexExtent,
				.mip_level_count = 1,
				.sample_count = 1,
				.dimension = WGPUTextureDimension_D2,
				.format = WGPUTextureFormat_Rgba8Unorm,
				.usage = WGPUTextureUsage_SAMPLED | WGPUTextureUsage_COPY_DST,
			});
		
		pTex->m_TexView = wgpu_texture_create_view(pTex->m_Tex, NULL);

		for(int i = 0; i < 4; i++)
			pTex->m_BindGroups[i] = 0;

		//dbg_msg("wgpu", "created texture: %llu", Tex);

		unsigned Bpp = 4;
		unsigned MemSize = Width * Height * Bpp;

		uint8_t *pStagingMem;
		WGPUBufferId TmpBuffer = wgpu_device_create_buffer_mapped(m_Device,
			&WGPUBufferDescriptor {
				.size = MemSize,
				.usage = WGPUBufferUsage_COPY_SRC},
			&pStagingMem);

		mem_copy(pStagingMem, pTexData, MemSize);
		wgpu_buffer_unmap(TmpBuffer);

		//dbg_msg("wgpu", "created buffer: %llu", TmpBuffer);

		EndRenderPass();

		WGPUCommandEncoderId CmdEncoder = GetCommandEncoder();

		wgpu_command_encoder_copy_buffer_to_texture(CmdEncoder, 
			&WGPUBufferCopyView {
				.buffer = TmpBuffer,
				.offset = 0,
				.bytes_per_row = Width * Bpp,
				.rows_per_image = 0
			},
			&WGPUTextureCopyView {
				.texture = pTex->m_Tex,
				.mip_level = 0,
				.array_layer = 0,
				.origin = { 0, 0, 0 }
			},
			TexExtent);

		wgpu_buffer_destroy(TmpBuffer);
		
		SubmitCommandBuffer();
		//dbg_msg("wgpu", "submitted copy command");

		/*if(!Mipmaps)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexImage2D(GL_TEXTURE_2D, 0, StoreWGPUFormat, Width, Height, 0, WGPUFormat, GL_UNSIGNED_BYTE, pTexData);
		}
		else
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			if(pCommand->m_Flags&CCommandBuffer::TEXTFLAG_LINEARMIPMAPS)
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			else
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
			glTexImage2D(GL_TEXTURE_2D, 0, StoreWGPUFormat, Width, Height, 0, WGPUFormat, GL_UNSIGNED_BYTE, pTexData);
		}*/

		// calculate memory usage
		m_aTextures[pCommand->m_Slot].m_MemSize += MemSize;
		if(Mipmaps)
		{
			int TexWidth = Width;
			int TexHeight = Height;
			while(TexWidth > 2 && TexHeight > 2)
			{
				TexWidth>>=1;
				TexHeight>>=1;
				m_aTextures[pCommand->m_Slot].m_MemSize += TexWidth * TexHeight * Bpp;
			}
		}
	}

	// 3D texture
	if(pCommand->m_Flags&CCommandBuffer::TEXFLAG_TEXTURE3D)
	{
		m_aTextures[pCommand->m_Slot].m_State |= CTexture::STATE_TEX3D;
		CTextureData *pTex = &m_aTextures[pCommand->m_Slot].m_Tex3D;

		Width /= IGraphics::NUMTILES_DIMENSION;
		Height /= IGraphics::NUMTILES_DIMENSION;
		Depth = IGraphics::NUMTILES_DIMENSION * IGraphics::NUMTILES_DIMENSION;

		// copy and reorder texture data
		unsigned Bpp = 4;
		unsigned MemSize = Width*Height*IGraphics::NUMTILES_DIMENSION*IGraphics::NUMTILES_DIMENSION*Bpp;
		unsigned char *pTmpData = (unsigned char *)mem_alloc(MemSize, sizeof(void*));

		const unsigned int TileSize = (Height * Width) * Bpp;
		const unsigned int TileRowSize = Width * Bpp;
		const unsigned int ImagePitch = Width * IGraphics::NUMTILES_DIMENSION * Bpp;
		mem_zero(pTmpData, MemSize);
		for(int i = 0; i < IGraphics::NUMTILES_DIMENSION * IGraphics::NUMTILES_DIMENSION; i++)
		{
			const int px = (i%IGraphics::NUMTILES_DIMENSION) * Width;
			const int py = (i/IGraphics::NUMTILES_DIMENSION) * Height;
			const unsigned char *pTileData = pTexData + (py * Width * IGraphics::NUMTILES_DIMENSION + px) * Bpp;
			for(int y = 0; y < Height; y++)
				mem_copy(pTmpData + i*TileSize + y*TileRowSize, pTileData + y * ImagePitch, TileRowSize);
		}

		mem_free(pTexData);
		pTexData = pTmpData;

		WGPUExtent3d TexExtent = {
			.width = (unsigned)Width,
			.height = (unsigned)Height,
			.depth = (unsigned)Depth,
		};
		pTex->m_Tex = wgpu_device_create_texture(m_Device, 
			&WGPUTextureDescriptor {
				.size = TexExtent,
				.mip_level_count = 1,
				.sample_count = 1,
				.dimension = WGPUTextureDimension_D2,
				.format = WGPUTextureFormat_Rgba8Unorm,
				.usage = WGPUTextureUsage_SAMPLED | WGPUTextureUsage_COPY_DST,
			});
		
		pTex->m_TexView = wgpu_texture_create_view(pTex->m_Tex, NULL);

		for(int i = 0; i < 4; i++)
			pTex->m_BindGroups[i] = 0;

		//dbg_msg("wgpu", "created texture: %llu", Tex);

		uint8_t *pStagingMem;
		WGPUBufferId TmpBuffer = wgpu_device_create_buffer_mapped(m_Device,
			&WGPUBufferDescriptor {
				.size = MemSize,
				.usage = WGPUBufferUsage_COPY_SRC},
			&pStagingMem);

		mem_copy(pStagingMem, pTexData, MemSize);
		wgpu_buffer_unmap(TmpBuffer);

		//dbg_msg("wgpu", "created buffer: %llu", TmpBuffer);

		EndRenderPass();

		WGPUCommandEncoderId CmdEncoder = GetCommandEncoder();

		TexExtent.depth = 1;

		for(unsigned i = 0; i < IGraphics::NUMTILES_DIMENSION * IGraphics::NUMTILES_DIMENSION; i++)
		{
			wgpu_command_encoder_copy_buffer_to_texture(CmdEncoder, 
				&WGPUBufferCopyView {
					.buffer = TmpBuffer,
					.offset = TileSize * i,
					.bytes_per_row = TileRowSize,
					.rows_per_image = 0
				},
				&WGPUTextureCopyView {
					.texture = pTex->m_Tex,
					.mip_level = 0,
					.array_layer = i,
					.origin = { 0, 0, 0 }
				},
				TexExtent);
		}

		wgpu_buffer_destroy(TmpBuffer);
		
		SubmitCommandBuffer();
		//dbg_msg("wgpu", "submitted copy command");

		m_aTextures[pCommand->m_Slot].m_MemSize += MemSize;
	}

	*m_pTextureMemoryUsage += m_aTextures[pCommand->m_Slot].m_MemSize;

	mem_free(pTexData);
}

void CCommandProcessorFragment_WGPU::Cmd_Clear(const CCommandBuffer::CClearCommand *pCommand)
{
	EndRenderPass();
	GetRenderPass(true, pCommand->m_Color);
}

void CCommandProcessorFragment_WGPU::Cmd_Render(const CCommandBuffer::CRenderCommand *pCommand)
{
	bool First = m_RPass == 0;
	WGPURenderPassId RPass = GetRenderPass();

	SetState(pCommand->m_State, pCommand->m_PrimType, RPass);

	unsigned PrimCount = 0;

	switch(pCommand->m_PrimType)
	{
	case CCommandBuffer::PRIMTYPE_TRIANGLES:
		PrimCount = pCommand->m_PrimCount*3;
		break;
	case CCommandBuffer::PRIMTYPE_LINES:
		PrimCount = pCommand->m_PrimCount*2;
		break;
	default:
		dbg_msg("render", "unknown primtype %d\n", pCommand->m_PrimType);
	};

	if(PrimCount > 0 && m_StreamingBuffer)
	{
		unsigned DataSize = PrimCount * sizeof(CCommandBuffer::CVertex);

		wgpu_render_pass_set_vertex_buffer(RPass, 0, m_StreamingBuffer, pCommand->m_Offset, DataSize);
		wgpu_render_pass_draw(RPass, PrimCount, 1, 0, 0);
	}
}

void CCommandProcessorFragment_WGPU::Cmd_Swap(const CCommandBuffer::CSwapCommand *pCommand)
{
	/*
	if(pCommand->m_Finish)
		glFinish();
	*/

	SubmitCommandBuffer();

	wgpu_swap_chain_present(m_SwapChain);

	m_NextTexture = wgpu_swap_chain_get_next_texture(m_SwapChain);
}

void CCommandProcessorFragment_WGPU::Cmd_Screenshot(const CCommandBuffer::CScreenshotCommand *pCommand)
{
	/*
	// fetch image data
	GLint aViewport[4] = {0,0,0,0};
	glGetIntegerv(GL_VIEWPORT, aViewport);

	int w = pCommand->m_W == -1 ? aViewport[2] : pCommand->m_W;
	int h = pCommand->m_H == -1 ? aViewport[3] : pCommand->m_H;
	int x = pCommand->m_X;
	int y = aViewport[3] - pCommand->m_Y - 1 - (h - 1);

	// we allocate one more row to use when we are flipping the texture
	unsigned char *pPixelData = (unsigned char *)mem_alloc(w*(h+1)*3, 1);
	unsigned char *pTempRow = pPixelData+w*h*3;

	// fetch the pixels
	GLint Alignment;
	glGetIntegerv(GL_PACK_ALIGNMENT, &Alignment);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, pPixelData);
	glPixelStorei(GL_PACK_ALIGNMENT, Alignment);

	// flip the pixel because WGPU works from bottom left corner
	for(int ty = 0; ty < h/2; ty++)
	{
		mem_copy(pTempRow, pPixelData+ty*w*3, w*3);
		mem_copy(pPixelData+ty*w*3, pPixelData+(h-ty-1)*w*3, w*3);
		mem_copy(pPixelData+(h-ty-1)*w*3, pTempRow,w*3);
	}

	// fill in the information
	pCommand->m_pImage->m_Width = w;
	pCommand->m_pImage->m_Height = h;
	pCommand->m_pImage->m_Format = CImageInfo::FORMAT_RGB;
	pCommand->m_pImage->m_pData = pPixelData;*/
}

CCommandProcessorFragment_WGPU::CCommandProcessorFragment_WGPU()
{
	mem_zero(m_aTextures, sizeof(m_aTextures));
	m_pTextureMemoryUsage = 0;
	m_CmdEncoder = 0;
	m_RPass = 0;
	m_StreamingBuffer = 0;
	m_TransformBindGroup = 0;
	m_Ready = false;
}

bool CCommandProcessorFragment_WGPU::RunCommand(const CCommandBuffer::CCommand * pBaseCommand)
{
	switch(pBaseCommand->m_Cmd)
	{
	case CMD_INIT: Cmd_Init(static_cast<const CInitCommand *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_TEXTURE_CREATE: Cmd_Texture_Create(static_cast<const CCommandBuffer::CTextureCreateCommand *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_TEXTURE_DESTROY: Cmd_Texture_Destroy(static_cast<const CCommandBuffer::CTextureDestroyCommand *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_TEXTURE_UPDATE: Cmd_Texture_Update(static_cast<const CCommandBuffer::CTextureUpdateCommand *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_CLEAR: Cmd_Clear(static_cast<const CCommandBuffer::CClearCommand *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_RENDER: Cmd_Render(static_cast<const CCommandBuffer::CRenderCommand *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_SWAP: Cmd_Swap(static_cast<const CCommandBuffer::CSwapCommand *>(pBaseCommand)); break;
	case CCommandBuffer::CMD_SCREENSHOT: Cmd_Screenshot(static_cast<const CCommandBuffer::CScreenshotCommand *>(pBaseCommand)); break;
	default: return false;
	}

	return true;
}

WGPURenderPipelineId CCommandProcessorFragment_WGPU::CreateRenderPipeline(WGPUPipelineLayoutId PipelineLayout, WGPUShaderModuleId VertexShader, WGPUShaderModuleId FragmentShader, int PrimType, WGPUBlendDescriptor BlendInfo)
{
	WGPUVertexAttributeDescriptor aVertexAttributes[3] = {
		{
			.offset = 0,
			.format = WGPUVertexFormat_Float2,
			.shader_location = 0,
		},
		{
			.offset = sizeof(float)*2,
			.format = WGPUVertexFormat_Float3,
			.shader_location = 1,
		},
		{
			.offset = sizeof(float)*5,
			.format = WGPUVertexFormat_Float4,
			.shader_location = 2,
		}
	};

	return wgpu_device_create_render_pipeline(m_Device,
		&WGPURenderPipelineDescriptor {
			.layout = PipelineLayout,
			.vertex_stage =
				WGPUProgrammableStageDescriptor {
					.module = VertexShader,
					.entry_point = "main",
				},
			.fragment_stage =
				&WGPUProgrammableStageDescriptor {
					.module = FragmentShader,
					.entry_point = "main",
				},
			.primitive_topology = PrimType == CCommandBuffer::PRIMTYPE_LINES ? WGPUPrimitiveTopology_LineList : WGPUPrimitiveTopology_TriangleList,
			.rasterization_state =
				&WGPURasterizationStateDescriptor {
					.front_face = WGPUFrontFace_Ccw,
					.cull_mode = WGPUCullMode_None,
					.depth_bias = 0,
					.depth_bias_slope_scale = 0.0,
					.depth_bias_clamp = 0.0,
				},
			.color_states =
				&WGPUColorStateDescriptor {
					.format = WGPUTextureFormat_Bgra8Unorm,
					.alpha_blend = BlendInfo,
					.color_blend = BlendInfo,
					.write_mask = WGPUColorWrite_ALL,
				},
			.color_states_length = 1,
			.depth_stencil_state = NULL,
			.vertex_state =
				WGPUVertexStateDescriptor {
					.index_format = WGPUIndexFormat_Uint16,
					.vertex_buffers = &WGPUVertexBufferLayoutDescriptor {
						.array_stride = sizeof(CCommandBuffer::CVertex),
						.step_mode = WGPUInputStepMode_Vertex,
						.attributes = aVertexAttributes,
						.attributes_length = 3,
					},
					.vertex_buffers_length = 1,
				},
			.sample_count = 1,
		});
}
	
WGPUSamplerId CCommandProcessorFragment_WGPU::CreateSampler(int WrapModeU, int WrapModeV)
{
	WGPUAddressMode ModeU = WrapModeU == IGraphics::WRAP_REPEAT
		? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
	WGPUAddressMode ModeV = WrapModeV == IGraphics::WRAP_REPEAT
		? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
	return wgpu_device_create_sampler(m_Device,
		&WGPUSamplerDescriptor {
			.address_mode_u = ModeU,
			.address_mode_v = ModeV,
			.address_mode_w = WGPUAddressMode_Repeat,
			.mag_filter = WGPUFilterMode_Linear,
			.min_filter = WGPUFilterMode_Linear,
			.mipmap_filter = WGPUFilterMode_Nearest,
			.lod_min_clamp = -100.f,
			.lod_max_clamp = 100.0,
			.compare = WGPUCompareFunction_Undefined,
		});
}

WGPUBindGroupId CCommandProcessorFragment_WGPU::GetTexBindGroup(CTextureData *pTex, int WrapModeU, int WrapModeV, bool Array)
{
	int WrapIndex = WrapModeU * 2 + WrapModeV;
	if(!pTex->m_BindGroups[WrapIndex])
	{
		WGPUBindGroupEntry aEntries[2] = {
			{
				.binding = 0,
				.resource = {
					.tag = WGPUBindingResource_TextureView,
					.texture_view = pTex->m_TexView,
				},
			},
			{
				.binding = 1,
				.resource = {
					.tag = WGPUBindingResource_Sampler,
					.sampler = m_Sampler[WrapIndex],
				},
			}
		};
		pTex->m_BindGroups[WrapIndex] = wgpu_device_create_bind_group(m_Device,
			&WGPUBindGroupDescriptor {
				.layout = Array ? m_BindGroup2DArrayLayout : m_BindGroup2DLayout,
				.entries = aEntries,
				.entries_length = 2,
			});
	}
	
	return pTex->m_BindGroups[WrapIndex];
}

WGPUCommandEncoderId CCommandProcessorFragment_WGPU::GetCommandEncoder()
{
	if(!m_CmdEncoder && m_Ready)
	{
		m_CmdEncoder = wgpu_device_create_command_encoder(
			m_Device, &WGPUCommandEncoderDescriptor {.label = "command encoder"});
		//dbg_msg("wgpu", "created command encoder");
	}

	return m_CmdEncoder;
}

WGPURenderPassId CCommandProcessorFragment_WGPU::GetRenderPass(bool Clear, CCommandBuffer::CColor ClearColor)
{
	if(!m_RPass && m_NextTexture.view_id)
	{
		WGPUCommandEncoderId CmdEncoder = GetCommandEncoder();

		WGPURenderPassColorAttachmentDescriptor aColorAttachments[1] = {
				{
					.attachment = m_NextTexture.view_id,
					.load_op = Clear ? WGPULoadOp_Clear : WGPULoadOp_Load,
					.store_op = WGPUStoreOp_Store,
					.clear_color = WGPUColor { .r = ClearColor.r, .g = ClearColor.g, .b = ClearColor.b, .a = 1.0 },
				},
			};

		m_RPass = wgpu_command_encoder_begin_render_pass(CmdEncoder,
			&WGPURenderPassDescriptor {
				.color_attachments = aColorAttachments,
				.color_attachments_length = 1,
				.depth_stencil_attachment = NULL,
			});

		//dbg_msg("wgpu", "began render pass");
	}

	return m_RPass;
}

void CCommandProcessorFragment_WGPU::EndRenderPass()
{
	if(m_RPass)
	{
		wgpu_render_pass_end_pass(m_RPass);
		//dbg_msg("wgpu", "render pass ended");
		m_RPass = 0;
	}
}

void CCommandProcessorFragment_WGPU::SubmitCommandBuffer()
{
	if(m_CmdEncoder)
	{
		EndRenderPass();
		WGPUCommandBufferId CmdBuf = wgpu_command_encoder_finish(m_CmdEncoder, NULL);
		//dbg_msg("wgpu", "finished command buffer");
		WGPUQueueId Queue = wgpu_device_get_default_queue(m_Device);
		wgpu_queue_submit(Queue, &CmdBuf, 1);
		//dbg_msg("wgpu", "submitted command buffer");
		m_CmdEncoder = 0;
	}
}

void CCommandProcessorFragment_WGPU::UploadStreamingData(const void *pData, unsigned Size, const CScreen *pScreens, int NumScreens)
{
	if(!m_StreamingBuffer)
	{
		uint8_t *pStagingMem;
		m_StreamingBuffer = wgpu_device_create_buffer_mapped(m_Device,
			&WGPUBufferDescriptor {
				.size = Size,
				.usage = WGPUBufferUsage_VERTEX},
			&pStagingMem);

		mem_copy(pStagingMem, pData, Size);
		wgpu_buffer_unmap(m_StreamingBuffer);

		//dbg_msg("wgpu", "uploaded streamig data: %u", Size);
	}
	else
	{
		dbg_msg("warning", "warning: streaming buffer");
	}

	if(!m_TransformBindGroup)
	{
		// TODO: use push constants instead
		unsigned DynAlignment = (sizeof(CMat4) + WGPUBIND_BUFFER_ALIGNMENT - 1) / WGPUBIND_BUFFER_ALIGNMENT * WGPUBIND_BUFFER_ALIGNMENT;
		unsigned MemSize = DynAlignment * NumScreens;

		uint8_t *pStagingMem;
		WGPUBufferId MatrixBuffer = wgpu_device_create_buffer_mapped(m_Device,
			&WGPUBufferDescriptor {
				.size = MemSize,
				.usage = WGPUBufferUsage_UNIFORM},
			&pStagingMem);

		for(int i = 0; i < NumScreens; i++)
		{
			const float Near = -1.0f;
			const float Far = 1.0f;

			CMat4 Ortho = {
				2.0f / (pScreens[i].BR_x - pScreens[i].TL_x), 0.0f, 0.0f, 0.0f,
				0.0f, 2.0f / (pScreens[i].TL_y - pScreens[i].BR_y), 0.0f, 0.0f,
				0.0f, 0.0f, 2.0f / (Far - Near) , 0.0f,
				- (pScreens[i].BR_x + pScreens[i].TL_x) / (pScreens[i].BR_x - pScreens[i].TL_x),
				- (pScreens[i].TL_y + pScreens[i].BR_y) / (pScreens[i].TL_y - pScreens[i].BR_y),
				- (Far + Near) / (Far - Near), 1.0f,
			};

			mem_copy(pStagingMem + DynAlignment * i, &Ortho, sizeof(CMat4));
		}
		wgpu_buffer_unmap(MatrixBuffer);

		WGPUBindGroupEntry aEntries[1] = {
			{
				.binding = 0,
				.resource = {
					.tag = WGPUBindingResource_Buffer,
					.buffer = {
						._0 = {
							.buffer = MatrixBuffer,
							.offset = 0,
							.size = sizeof(CMat4),
						}
					}
				},
			}
		};
		m_TransformBindGroup = wgpu_device_create_bind_group(m_Device,
			&WGPUBindGroupDescriptor {
				.layout = m_BindGroupTransformLayout,
				.entries = aEntries,
				.entries_length = 1,
			});

		wgpu_buffer_destroy(MatrixBuffer);

		m_ScreenCount = 0;
	}
	else
	{
		dbg_msg("warning", "warning: transform buffer");
	}
}

void CCommandProcessorFragment_WGPU::FreeStreamingData()
{
	if(m_StreamingBuffer)
	{
		wgpu_buffer_destroy(m_StreamingBuffer);
		//dbg_msg("wgpu", "freed streamig data");
		m_StreamingBuffer = 0;
	}

	if(m_TransformBindGroup)
	{
		wgpu_bind_group_destroy(m_TransformBindGroup);
		m_TransformBindGroup = 0;
	}
}


// ------------ CCommandProcessor_SDL_WGPU

void CCommandProcessor_SDL_WGPU::RunBuffer(CCommandBuffer *pBuffer)
{
	unsigned CmdIndex = 0;
	array<CScreen> Screens;

	while(1)
	{
		const CCommandBuffer::CCommand *pBaseCommand = pBuffer->GetCommand(&CmdIndex);
		if(pBaseCommand == 0x0)
			break;

		if(pBaseCommand->m_Cmd == CCommandBuffer::CMD_RENDER)
		{
			const CCommandBuffer::CRenderCommand *pRenderCmd = static_cast<const CCommandBuffer::CRenderCommand *>(pBaseCommand);
			CScreen CurScreen = {
				pRenderCmd->m_State.m_ScreenTL.x,
				pRenderCmd->m_State.m_ScreenTL.y,
				pRenderCmd->m_State.m_ScreenBR.x,
				pRenderCmd->m_State.m_ScreenBR.y
			};
			if(Screens.size() == 0 || mem_comp(&Screens[Screens.size()-1], &CurScreen, sizeof(CurScreen)) != 0)
				Screens.add(CurScreen);
		}
	}

	if(pBuffer->DataUsed() > 0)
		m_WGPU.UploadStreamingData(pBuffer->DataPtr(), pBuffer->DataUsed(), &Screens[0], Screens.size());
	else
		dbg_msg("", "no streaming data");

	CmdIndex = 0;
	while(1)
	{
		const CCommandBuffer::CCommand *pBaseCommand = pBuffer->GetCommand(&CmdIndex);
		if(pBaseCommand == 0x0)
			break;

		if(m_WGPU.RunCommand(pBaseCommand))
			continue;

		if(m_General.RunCommand(pBaseCommand))
			continue;

		dbg_msg("graphics", "unknown command %d", pBaseCommand->m_Cmd);
	}
	m_WGPU.SubmitCommandBuffer();
	m_WGPU.FreeStreamingData();
}

// ------------ CGraphicsBackend_SDL_WGPU

int CGraphicsBackend_SDL_WGPU::Init(const char *pName, int *pScreen, int *pWindowWidth, int *pWindowHeight, int* pScreenWidth, int* pScreenHeight, int FsaaSamples, int Flags, int *pDesktopWidth, int *pDesktopHeight)
{
	if(!SDL_WasInit(SDL_INIT_VIDEO))
	{
		if(SDL_InitSubSystem(SDL_INIT_VIDEO) < 0)
		{
			dbg_msg("gfx", "unable to init SDL video: %s", SDL_GetError());
			return -1;
		}
	}

	// set screen
	SDL_Rect ScreenPos;
	m_NumScreens = SDL_GetNumVideoDisplays();
	if(m_NumScreens > 0)
	{
		*pScreen = clamp(*pScreen, 0, m_NumScreens-1);
		if(SDL_GetDisplayBounds(*pScreen, &ScreenPos) != 0)
		{
			dbg_msg("gfx", "unable to retrieve screen information: %s", SDL_GetError());
			return -1;
		}
	}
	else
	{
		dbg_msg("gfx", "unable to retrieve number of screens: %s", SDL_GetError());
		return -1;
	}

	// store desktop resolution for settings reset button
	if(!GetDesktopResolution(*pScreen, pDesktopWidth, pDesktopHeight))
	{
		dbg_msg("gfx", "unable to get desktop resolution: %s", SDL_GetError());
		return -1;
	}

	// use desktop resolution as default resolution
	if (*pWindowWidth == 0 || *pWindowHeight == 0)
	{
		*pWindowWidth = *pDesktopWidth;
		*pWindowHeight = *pDesktopHeight;
	}

	// set flags
	int SdlFlags = 0;
	if(Flags&IGraphicsBackend::INITFLAG_HIGHDPI)
		SdlFlags |= SDL_WINDOW_ALLOW_HIGHDPI;
	if(Flags&IGraphicsBackend::INITFLAG_RESIZABLE)
		SdlFlags |= SDL_WINDOW_RESIZABLE;
	if(Flags&IGraphicsBackend::INITFLAG_BORDERLESS)
		SdlFlags |= SDL_WINDOW_BORDERLESS;
	if(Flags&IGraphicsBackend::INITFLAG_FULLSCREEN)
#if defined(CONF_PLATFORM_MACOSX)	// Todo SDL: remove this when fixed (game freezes when losing focus in fullscreen)
	{
		SdlFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;	// always use "fake" fullscreen
		*pWindowWidth = *pDesktopWidth;
		*pWindowHeight = *pDesktopHeight;
	}
#else
		SdlFlags |= SDL_WINDOW_FULLSCREEN;
#endif

	if(Flags&IGraphicsBackend::INITFLAG_X11XRANDR)
		SDL_SetHint(SDL_HINT_VIDEO_X11_XRANDR, "1");

	/*// set gl attributes
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	if(FsaaSamples)
	{
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, FsaaSamples);
	}
	else
	{
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
	}*/

	// calculate centered position in windowed mode
	int OffsetX = 0;
	int OffsetY = 0;
	if(!(Flags&IGraphicsBackend::INITFLAG_FULLSCREEN) && *pDesktopWidth > *pWindowWidth && *pDesktopHeight > *pWindowHeight)
	{
		OffsetX = (*pDesktopWidth - *pWindowWidth) / 2;
		OffsetY = (*pDesktopHeight - *pWindowHeight) / 2;
	}

	// create window
	m_pWindow = SDL_CreateWindow(pName, ScreenPos.x+OffsetX, ScreenPos.y+OffsetY, *pWindowWidth, *pWindowHeight, SdlFlags);
	if(m_pWindow == NULL)
	{
		dbg_msg("gfx", "unable to create window: %s", SDL_GetError());
		return -1;
	}

	SDL_GetWindowSize(m_pWindow, pWindowWidth, pWindowHeight);

	SDL_SysWMinfo WmInfo;
	SDL_VERSION(&WmInfo.version);
	SDL_GetWindowWMInfo(m_pWindow, &WmInfo);
	HWND hwnd = WmInfo.info.win.window;

	WGPUSurfaceId Surface;

	SDL_GL_GetDrawableSize(m_pWindow, pScreenWidth, pScreenHeight); // drawable size may differ in high dpi mode

	#if defined(CONF_FAMILY_WINDOWS)
	Surface = wgpu_create_surface_from_windows_hwnd(WmInfo.info.win.hinstance, WmInfo.info.win.window);
	#else
		#if defined(CONF_PLATFORM_MACOSX)
		// TODO
		#else
		surface = wgpu_create_surface_from_xlib((const void **)WmInfo.info.x11.display, WmInfo.info.x11.window);
		#endif
	#endif

	//SDL_GL_GetDrawableSize(m_pWindow, pScreenWidth, pScreenHeight); // drawable size may differ in high dpi mode

	*pScreenWidth = *pWindowWidth;
	*pScreenHeight = *pWindowHeight;

	WGPUAdapterId Adapter = { 0 };
	wgpu_request_adapter_async(
		&WGPURequestAdapterOptions {
			.power_preference = WGPUPowerPreference_HighPerformance,
			.compatible_surface = Surface,
		},
		2, // 2 | 4 | 8,
		FequestAdapterCallback,
		(void *) &Adapter
	);

	dbg_msg("wgpu", "adapter requested");

	m_Device = wgpu_adapter_request_device(Adapter,
		&WGPUDeviceDescriptor {
			.extensions =
				{
					.anisotropic_filtering = false,
				},
			.limits =
				{
					.max_bind_groups = 2,
				},
		});

	dbg_msg("wgpu", "device requested");

	// print sdl version
	SDL_version Compiled;
	SDL_version Linked;

	SDL_VERSION(&Compiled);
	SDL_GetVersion(&Linked);
	dbg_msg("sdl", "SDL version %d.%d.%d (dll = %d.%d.%d)", Compiled.major, Compiled.minor, Compiled.patch, Linked.major, Linked.minor, Linked.patch);

	// start the command processor
	m_pProcessor = new CCommandProcessor_SDL_WGPU;
	StartProcessor(m_pProcessor);

	// issue init commands for WGPU and SDL
	CCommandBuffer CmdBuffer(1024, 512);
	CCommandProcessorFragment_WGPU::CInitCommand CmdWGPU;
	CmdWGPU.m_Device = m_Device;
	CmdWGPU.m_Surface = Surface;
	CmdWGPU.m_ScreenWidth = *pScreenWidth;
	CmdWGPU.m_ScreenHeight = *pScreenHeight;
	CmdWGPU.m_pTextureMemoryUsage = &m_TextureMemoryUsage;
	CmdBuffer.AddCommand(CmdWGPU);
	RunBuffer(&CmdBuffer);
	WaitForIdle();

	// return
	return 0;
}

int CGraphicsBackend_SDL_WGPU::Shutdown()
{
	// TODO: issue a shutdown command

	// stop and delete the processor
	StopProcessor();
	delete m_pProcessor;
	m_pProcessor = 0;

	SDL_DestroyWindow(m_pWindow);
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	return 0;
}

int CGraphicsBackend_SDL_WGPU::MemoryUsage() const
{
	return m_TextureMemoryUsage;
}

void CGraphicsBackend_SDL_WGPU::Minimize()
{
	SDL_MinimizeWindow(m_pWindow);
}

void CGraphicsBackend_SDL_WGPU::Maximize()
{
	SDL_MaximizeWindow(m_pWindow);
}

bool CGraphicsBackend_SDL_WGPU::Fullscreen(bool State)
{
#if defined(CONF_PLATFORM_MACOSX)	// Todo SDL: remove this when fixed (game freezes when losing focus in fullscreen)
	return SDL_SetWindowFullscreen(m_pWindow, State ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0;
#else
	return SDL_SetWindowFullscreen(m_pWindow, State ? SDL_WINDOW_FULLSCREEN : 0) == 0;
#endif
}

void CGraphicsBackend_SDL_WGPU::SetWindowBordered(bool State)
{
	SDL_SetWindowBordered(m_pWindow, SDL_bool(State));
}

bool CGraphicsBackend_SDL_WGPU::SetWindowScreen(int Index)
{
	if(Index >= 0 && Index < m_NumScreens)
	{
		SDL_Rect ScreenPos;
		if(SDL_GetDisplayBounds(Index, &ScreenPos) == 0)
		{
			SDL_SetWindowPosition(m_pWindow, ScreenPos.x, ScreenPos.y);
			return true;
		}
	}

	return false;
}

int CGraphicsBackend_SDL_WGPU::GetWindowScreen()
{
	return SDL_GetWindowDisplayIndex(m_pWindow);
}

int CGraphicsBackend_SDL_WGPU::GetVideoModes(CVideoMode *pModes, int MaxModes, int Screen)
{
	int NumModes = SDL_GetNumDisplayModes(Screen);
	if(NumModes < 0)
	{
		dbg_msg("gfx", "unable to get the number of display modes: %s", SDL_GetError());
		return 0;
	}

	if(NumModes > MaxModes)
		NumModes = MaxModes;

	int ModesCount = 0;
	for(int i = 0; i < NumModes; i++)
	{
		SDL_DisplayMode Mode;
		if(SDL_GetDisplayMode(Screen, i, &Mode) < 0)
		{
			dbg_msg("gfx", "unable to get display mode: %s", SDL_GetError());
			continue;
		}

		bool AlreadyFound = false;
		for(int j = 0; j < ModesCount; j++)
		{
			if(pModes[j].m_Width == Mode.w && pModes[j].m_Height == Mode.h)
			{
				AlreadyFound = true;
				break;
			}
		}
		if(AlreadyFound)
			continue;

		pModes[ModesCount].m_Width = Mode.w;
		pModes[ModesCount].m_Height = Mode.h;
		pModes[ModesCount].m_Red = 8;
		pModes[ModesCount].m_Green = 8;
		pModes[ModesCount].m_Blue = 8;
		ModesCount++;
	}
	return ModesCount;
}

bool CGraphicsBackend_SDL_WGPU::GetDesktopResolution(int Index, int *pDesktopWidth, int* pDesktopHeight)
{
	SDL_DisplayMode DisplayMode;
	if(SDL_GetDesktopDisplayMode(Index, &DisplayMode))
		return false;

	*pDesktopWidth = DisplayMode.w;
	*pDesktopHeight = DisplayMode.h;
	return true;
}

int CGraphicsBackend_SDL_WGPU::WindowActive()
{
	return SDL_GetWindowFlags(m_pWindow)&SDL_WINDOW_INPUT_FOCUS;
}

int CGraphicsBackend_SDL_WGPU::WindowOpen()
{
	return SDL_GetWindowFlags(m_pWindow)&SDL_WINDOW_SHOWN;

}


IGraphicsBackend *CreateGraphicsBackend() { return new CGraphicsBackend_SDL_WGPU; }
