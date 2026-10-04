#include <simulator/g3_draw.h>

#include <simulator/g3_handler.h>
#include <simulator/assert.h>
#include <string.h>
#include <stddef.h>

#include <nitro.h>
#include <nitro/gx/g3.h>
#include <nitro/gx/gx_load.h>
#include <nitro/gx/gx_vramcnt.h>
#include <unordered_map>
#include <simulator/sim_crc32.h>

#ifdef SDK_BUILD_LINUX
#include <signal.h>
#endif

#ifdef SDK_TRACY_ENABLE
#include "tracy/TracyC.h"

// Per-frame counters, shown as plots in Tracy (see G3SIM_DrawStatsEndFrame)
static u32 sStatFlushes;
static u32 sStatTexUploads;
static u32 sStatTexUploadBytes;
static u32 sStatDraws;
static u32 sStatVertexUploadBytes;
static u32 sStatTranslucentItems;
static u32 sStatTexCacheMisses;
#define G3_STAT_ADD(stat, n) ((stat) += (n))
#else
#define G3_STAT_ADD(stat, n) ((void)0)
#endif

#define G3_DRAW_MAX_ITEMS 1000

#define getbit(x,n) ( ( (x) >> (n) ) & 1 )

// One converted texture on the GPU, shared by every draw that uses it
typedef struct {
	GLuint textureId;
	u32 lastUsedFrame;
	u8 mirroredS; // Current GL_TEXTURE_WRAP_S is GL_MIRRORED_REPEAT (1) or GL_REPEAT (0)
	u8 mirroredT; // Same for GL_TEXTURE_WRAP_T
} g3_cached_texture_t;

// Everything that changes how a texture looks. Two textures are the same
// only if all of these match.
struct G3TextureKey {
	u32 textureCRC;
	u32 paletteCRC;
	u16 sSize;
	u16 tSize;
	u32 format;

	bool operator==(const G3TextureKey &other) const {
		return textureCRC == other.textureCRC && paletteCRC == other.paletteCRC
			&& sSize == other.sSize && tSize == other.tSize && format == other.format;
	}
};

struct G3TextureKeyHash {
	size_t operator()(const G3TextureKey &key) const {
		u64 h = ((u64)key.textureCRC << 32) | key.paletteCRC;
		h ^= ((u64)key.sSize << 48) ^ ((u64)key.tSize << 32) ^ key.format;
		return std::hash<u64>{}(h);
	}
};

typedef struct {
	float zIdx;
	g3_cached_texture_t * texture; // NULL when the item has no texture
	G3SIM_PolygonAttr_t polygonAttr;
	G3SIM_TexImageParam_t texImageParam;
	G3SIM_Vertex_t * verts;
	u32 vertsCount;
} g3_draw_item_t;

G3SIM_Vertex_t s_G3DrawVerts[G3_DRAW_MAX_VERTS];

// Texture cache. Each different texture is converted and uploaded to the
// GPU once, then reused by binding it. Uploading again into a texture the
// GPU may still be drawing with makes the driver wait, which used to be the
// biggest cost per frame in busy scenes.
static std::unordered_map<G3TextureKey, g3_cached_texture_t, G3TextureKeyHash> sTextureCache;

// Counts frames, to know which textures have not been used for a while
static u32 sDrawFrame = 0;

// Above this many cached textures, delete the ones not used recently
#define G3_TEXTURE_CACHE_MAX_COUNT 2048
// A texture used within this many frames is never deleted
#define G3_TEXTURE_CACHE_KEEP_FRAMES 600

//The item list is used for drawing translucent things sorted by Z position
static g3_draw_item_t s_G3DrawItemList[G3_DRAW_MAX_ITEMS];
static u32 s_G3DrawItemListCount = 0;

static u32 s_G3DrawCurVertIdx = 0;
static GLuint s_G3DrawVertexArray;
static GLuint s_G3DrawVertexBuffer;

extern G3SIM_TexImageParam_t s_texImageParam;
extern G3SIM_PolygonAttr_t s_curPolygonAttr;
extern GLuint g3shaderProgramID;
extern u8 s_SIM_g3tex[4*1024*1024];
extern GXVRamTex s_SIM_GXVRamTex;
extern GXVRamTexPltt s_SIM_GXVRamTexPltt;


static void* getTextureVramBank();
static void* getTexPlttVramBank();

static void* getTextureVramBank()
{
	void * vramBank = NULL;
	switch( s_SIM_GXVRamTex ){
		case GX_VRAM_TEX_0_A:
		case GX_VRAM_TEX_01_AB:
		case GX_VRAM_TEX_012_ABC:
		case GX_VRAM_TEX_0123_ABCD:
		case GX_VRAM_TEX_01_AC:
		case GX_VRAM_TEX_01_AD:
		case GX_VRAM_TEX_012_ABD:
		case GX_VRAM_TEX_012_ACD:
			vramBank = (void *)HW_LCDC_VRAM_A;
			break;
		case GX_VRAM_TEX_0_B:
		case GX_VRAM_TEX_01_BC:
		case GX_VRAM_TEX_012_BCD:
		case GX_VRAM_TEX_01_BD:
			vramBank = (void *)HW_LCDC_VRAM_B;
			break;
		case GX_VRAM_TEX_0_C:
		case GX_VRAM_TEX_01_CD:
			vramBank = (void *)HW_LCDC_VRAM_C;
			break;
		case GX_VRAM_TEX_0_D:
			vramBank = (void *)HW_LCDC_VRAM_A;
			break;
		default:
			//?
			break;
	}
	
	return vramBank;
}

static void* getTexPlttVramBank()
{
	void * vramBank = NULL;
	switch( s_SIM_GXVRamTexPltt ) {
		case GX_VRAM_TEXPLTT_NONE:
		case GX_VRAM_TEXPLTT_0123_E:
		case GX_VRAM_TEXPLTT_01234_EF:
		case GX_VRAM_TEXPLTT_012345_EFG:
			vramBank = (void *)HW_LCDC_VRAM_E;
			break;
		case GX_VRAM_TEXPLTT_0_F:
		case GX_VRAM_TEXPLTT_01_FG:
			vramBank = (void *)HW_LCDC_VRAM_F;
			break;
		case GX_VRAM_TEXPLTT_0_G:
			vramBank = (void *)HW_LCDC_VRAM_G;
			break;
	}
	
	return vramBank;
}

void G3SIM_AddVtx(G3SIM_Vertex_t * vtx)
{
	memcpy(&s_G3DrawVerts[s_G3DrawCurVertIdx], vtx, sizeof(G3SIM_Vertex_t));

	s_G3DrawCurVertIdx = s_G3DrawCurVertIdx + 1;

	SIM_assert(s_G3DrawCurVertIdx < G3_DRAW_MAX_VERTS);
	return;
}

void G3SIM_DrawArray()
{
	if( s_G3DrawCurVertIdx != 0 )
	{
		glBindVertexArray(s_G3DrawVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
		#ifdef SDK_TRACY_ENABLE
		TracyCZoneN(DrawArrayZone, "G3 vertex upload + draw", 1);
		#endif
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx, s_G3DrawVerts);
		glDrawArrays(GL_TRIANGLES,0,s_G3DrawCurVertIdx);
		G3_STAT_ADD(sStatDraws, 1);
		G3_STAT_ADD(sStatVertexUploadBytes, sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx);
		#ifdef SDK_TRACY_ENABLE
		TracyCZoneEnd(DrawArrayZone);
		#endif
	}
	return;
}

void G3SIM_DrawCleanUp()
{
	s_G3DrawCurVertIdx = 0;
}

void G3SIM_DrawInit()
{
	glGenVertexArrays(1, &s_G3DrawVertexArray);
	glBindVertexArray(s_G3DrawVertexArray);

	glGenBuffers(1, &s_G3DrawVertexBuffer);

	glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(s_G3DrawVerts), NULL, GL_DYNAMIC_DRAW);


	glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, s));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(G3SIM_Vertex_t), (void*)offsetof(G3SIM_Vertex_t, r));

	glBindVertexArray(0);
	glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
}

// Find the current texture (s_texImageParam) in the cache, converting and
// uploading it first if it is not there yet. Leaves it bound to GL_TEXTURE0.
static g3_cached_texture_t * G3SIM_GetCurrentTexture()
{
	u8 * vramTex = (u8*)(getTextureVramBank() + s_texImageParam.textureOffset);
	u8 * vramPltt = (u8*)(getTexPlttVramBank() + s_texPlttBase);
	u16 * colorAddr = (u16*)vramPltt;
	u32 sSize = s_texImageParam.textureSSize;
	u32 tSize = s_texImageParam.textureTSize;

	u32 vramTexBufSize = 0;
	if(s_texImageParam.textureFormat == GX_TEXFMT_PLTT4) {
		vramTexBufSize = (sSize * tSize) >> 2;
	} else if(s_texImageParam.textureFormat == GX_TEXFMT_PLTT16) {
		vramTexBufSize = (sSize * tSize) >> 1;
	} else if(s_texImageParam.textureFormat == GX_TEXFMT_DIRECT) {
		vramTexBufSize = (sSize * tSize) * 2; // 16 bits per pixel
	} else {
		vramTexBufSize = (sSize * tSize);
	}

	G3TextureKey key;
	key.textureCRC = SIM_crc32buf(vramTex, vramTexBufSize);
	// Direct color textures do not use a palette
	key.paletteCRC = (s_texImageParam.textureFormat == GX_TEXFMT_DIRECT) ? 0 : SIM_crc32buf(vramPltt, 512);
	key.sSize = sSize;
	key.tSize = tSize;
	key.format = s_texImageParam.textureFormat;

	glActiveTexture(GL_TEXTURE0);

	auto found = sTextureCache.find(key);
	if(found != sTextureCache.end()) {
		found->second.lastUsedFrame = sDrawFrame;
		glBindTexture(GL_TEXTURE_2D, found->second.textureId);
		return &found->second;
	}

	G3_STAT_ADD(sStatTexCacheMisses, 1);

	//Convert the DS texture data into a format opengl can understand
	u8 * outTexBuf = new u8[4 * sSize * tSize];
	memset((void*)outTexBuf, 0, 4 * sSize * tSize);

	switch( s_texImageParam.textureFormat ){
		case GX_TEXFMT_A3I5:
			G3SIM_DecodeTexA3I5(vramTex, colorAddr, outTexBuf, sSize, tSize);
			break;
		case GX_TEXFMT_PLTT4:
			G3SIM_DecodeTex4(vramTex, colorAddr, outTexBuf, sSize, tSize);
			break;
		case GX_TEXFMT_PLTT16:
			G3SIM_DecodeTex16(vramTex, colorAddr, outTexBuf, sSize, tSize);
			break;
		case GX_TEXFMT_PLTT256:
			G3SIM_DecodeTex256(vramTex, colorAddr, outTexBuf, sSize, tSize);
			break;
		case GX_TEXFMT_COMP4x4:
			SIM_assert_always();
			//TODO
			break;
		case GX_TEXFMT_A5I3:
			G3SIM_DecodeTexA5I3(vramTex, colorAddr, outTexBuf, sSize, tSize);
			break;
		case GX_TEXFMT_DIRECT:
			G3SIM_DecodeTexDirect(vramTex, outTexBuf, sSize, tSize);
			break;
	}

	g3_cached_texture_t texture;
	texture.lastUsedFrame = sDrawFrame;
	texture.mirroredS = 0;
	texture.mirroredT = 0;

	#ifdef SDK_TRACY_ENABLE
	TracyCZoneN(TexUploadZone, "G3 texture upload", 1);
	#endif
	glGenTextures(1, &texture.textureId);
	glBindTexture(GL_TEXTURE_2D, texture.textureId);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sSize, tSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, (void *)outTexBuf);
	G3_STAT_ADD(sStatTexUploads, 1);
	G3_STAT_ADD(sStatTexUploadBytes, 4 * sSize * tSize);
	#ifdef SDK_TRACY_ENABLE
	TracyCZoneEnd(TexUploadZone);
	#endif

	delete[] outTexBuf;

	return &(sTextureCache[key] = texture);
}

// Set the wrap mode of a bound cached texture, only when it changes
static void G3SIM_SetTextureWrap(g3_cached_texture_t * texture, BOOL flipS, BOOL flipT)
{
	u8 mirroredS = flipS ? 1 : 0;
	u8 mirroredT = flipT ? 1 : 0;

	if(texture->mirroredS != mirroredS) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mirroredS ? GL_MIRRORED_REPEAT : GL_REPEAT);
		texture->mirroredS = mirroredS;
	}
	if(texture->mirroredT != mirroredT) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mirroredT ? GL_MIRRORED_REPEAT : GL_REPEAT);
		texture->mirroredT = mirroredT;
	}
}

// Delete textures that have not been used for a while, once the cache is big
static void G3SIM_TrimTextureCache()
{
	if(sTextureCache.size() <= G3_TEXTURE_CACHE_MAX_COUNT) {
		return;
	}

	for(auto it = sTextureCache.begin(); it != sTextureCache.end(); ) {
		if(sDrawFrame - it->second.lastUsedFrame > G3_TEXTURE_CACHE_KEEP_FRAMES) {
			glDeleteTextures(1, &it->second.textureId);
			it = sTextureCache.erase(it);
		} else {
			++it;
		}
	}
}

void G3SIM_FlushArray()
{
	if(s_G3DrawCurVertIdx == 0) {
		// No verts to draw
		return;
	}

	#ifdef SDK_TRACY_ENABLE
	TracyCZone(FlushArrayZone, 1);
	#endif
	G3_STAT_ADD(sStatFlushes, 1);

	g3_cached_texture_t * texture = nullptr;

	if( s_texImageParam.textureFormat != GX_TEXFMT_NONE )
	{
		texture = G3SIM_GetCurrentTexture();
		G3SIM_SetTextureWrap(texture, s_texImageParam.flipS, s_texImageParam.flipT);

		GLint texUnitLoc = glGetUniformLocation(g3shaderProgramID, "myTexture");
		//set texture 0 in the shader
		glProgramUniform1i(g3shaderProgramID, texUnitLoc , 0);

		//Enable texture in the shader
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 1);
	}
	else
	{
		//Disable texture in the shader
		glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 0);							
	}

	// Set the polygon mode in the shader
	GLint polygonModeLoc = glGetUniformLocation(g3shaderProgramID, "polygonMode");
	glProgramUniform1i(g3shaderProgramID, polygonModeLoc , s_curPolygonAttr.polygonMode);

	// Set the fog attributes in the shader
	GLint useFogLoc = glGetUniformLocation(g3shaderProgramID, "useFog");
	int useFog = getbit(s_reg_G3X_DISP3DCNT, 7);
	if(!s_curPolygonAttr.fogEnable) {
		useFog = 0;
	}
	glProgramUniform1i(g3shaderProgramID, useFogLoc , useFog);
	if(useFog) {
		// Fog color
		GLint fogColorLoc = glGetUniformLocation(g3shaderProgramID, "fogColor");
		u32 fogRed = s_reg_G3X_FOG_COLOR & 0b11111;
		u32 fogGreen = (s_reg_G3X_FOG_COLOR >> 5) & 0b11111;
		u32 fogBlue = (s_reg_G3X_FOG_COLOR >> 10) & 0b11111;
		u32 fogAlpha = (s_reg_G3X_FOG_COLOR >> 16) & 0b11111;
		float fogRedF = (float)fogRed / 32.0f;
		float fogGreenF = (float)fogGreen / 32.0f;
		float fogBlueF = (float)fogBlue / 32.0f;
		float fogAlphaF = (float)fogAlpha / 32.0f;
		glProgramUniform4f(g3shaderProgramID, fogColorLoc, fogRedF, fogGreenF, fogBlueF, fogAlphaF);


		u32 fogShift = (s_reg_G3X_DISP3DCNT >> 8) & 0b1111;
		u32 fogStep = 0x400 >> fogShift;

		//Fog depth boundaries
		u32 fogOffset = s_reg_G3X_FOG_OFFSET;

		float fogDepthBoundary[32];
		float fogDensity[32];
		u8 * fogTablePtr = (u8*)&s_reg_G3X_FOG_TABLE_0;
		for(int i=0; i < 32; i++) {
			fogDepthBoundary[i] = ((float)(fogOffset + fogStep*(i+1)) / 32767.0f) * 2.0f - 1.0f;
			fogDensity[i] = (float)fogTablePtr[i] / 127.0f;
		}

		GLint fogDepthBoundaryLoc = glGetUniformLocation(g3shaderProgramID, "fogDepthBoundary");
		glProgramUniform1fv(g3shaderProgramID, fogDepthBoundaryLoc, 32, fogDepthBoundary);

		GLint fogDensityLoc = glGetUniformLocation(g3shaderProgramID, "fogDensity");
		glProgramUniform1fv(g3shaderProgramID, fogDensityLoc, 32, fogDensity);
	}



	// If this is a translucent draw, save it for the end of the frame to render it last
	if(
		( s_texImageParam.textureFormat == GX_TEXFMT_A3I5 
	   || s_texImageParam.textureFormat == GX_TEXFMT_A5I3
	   || s_curPolygonAttr.alpha < 0.99f )
	 && s_G3DrawCurVertIdx != 0) {
		g3_draw_item_t * item = &s_G3DrawItemList[s_G3DrawItemListCount];

		memset(item, 0, sizeof(g3_draw_item_t));


		// The cached texture stays alive until at least the end of this
		// frame, so the item can just point at it
		item->texture = texture;
		G3_STAT_ADD(sStatTranslucentItems, 1);

		//Copy over the polygonattr
		memcpy(&item->polygonAttr, &s_curPolygonAttr, sizeof(G3SIM_PolygonAttr_t));

		//Copy over the texImageParam
		memcpy(&item->texImageParam, &s_texImageParam, sizeof(G3SIM_TexImageParam_t));

		//Allocate and copy the verts
		item->verts = (G3SIM_Vertex_t*)(malloc(sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx));
		item->vertsCount = s_G3DrawCurVertIdx;
		memcpy(item->verts, s_G3DrawVerts, sizeof(G3SIM_Vertex_t) * s_G3DrawCurVertIdx);

		s_G3DrawItemListCount++;
	} else {
		G3SIM_DrawArray();
	}

	G3SIM_DrawCleanUp();
	#ifdef SDK_TRACY_ENABLE
	TracyCZoneEnd(FlushArrayZone);
	#endif
}

void G3SIM_DrawItems()
{
	//TODO sort the items by Z coordinate


	//Draws all the translucent objects that were deferred to the end of the frame.
	for(int i=0; i<s_G3DrawItemListCount; i++) {
		g3_draw_item_t * item = &s_G3DrawItemList[i];
		if(item->texture != nullptr){
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, item->texture->textureId);
			G3SIM_SetTextureWrap(item->texture, item->texImageParam.flipS, item->texImageParam.flipT);
			//Enable texture in the shader
			glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 1);
		} else {
			glProgramUniform1i(g3shaderProgramID, glGetUniformLocation(g3shaderProgramID, "useTexture") , 0);
		}

		// Set the polygon mode in the shader
		GLint polygonModeLoc = glGetUniformLocation(g3shaderProgramID, "polygonMode");
		glProgramUniform1i(g3shaderProgramID, polygonModeLoc, item->polygonAttr.polygonMode);

		glBindVertexArray(s_G3DrawVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, s_G3DrawVertexBuffer);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(G3SIM_Vertex_t) * item->vertsCount, item->verts);
		glDrawArrays(GL_TRIANGLES,0, item->vertsCount);
		G3_STAT_ADD(sStatDraws, 1);
		G3_STAT_ADD(sStatVertexUploadBytes, sizeof(G3SIM_Vertex_t) * item->vertsCount);

		free(item->verts);
	}
	s_G3DrawItemListCount = 0;

	// This runs once per frame, at the start of SIM_Render
	sDrawFrame++;
	G3SIM_TrimTextureCache();
}
#ifdef SDK_TRACY_ENABLE
// Send this frame's counters to Tracy and reset them. Call once per frame.
void G3SIM_DrawStatsEndFrame()
{
	TracyCPlot("G3 flushes", sStatFlushes);
	TracyCPlot("G3 draws", sStatDraws);
	TracyCPlot("G3 texture uploads", sStatTexUploads);
	TracyCPlot("G3 texture upload KB", sStatTexUploadBytes / 1024.0);
	TracyCPlot("G3 vertex upload KB", sStatVertexUploadBytes / 1024.0);
	TracyCPlot("G3 translucent items", sStatTranslucentItems);
	TracyCPlot("G3 texture cache misses", sStatTexCacheMisses);
	sStatFlushes = 0;
	sStatTexUploads = 0;
	sStatTexUploadBytes = 0;
	sStatDraws = 0;
	sStatVertexUploadBytes = 0;
	sStatTranslucentItems = 0;
	sStatTexCacheMisses = 0;
}
#endif
