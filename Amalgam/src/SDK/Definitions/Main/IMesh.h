#pragma once
#include "../Interfaces/IMaterialSystem.h"

// Minimal port of Source's materialsystem/imesh.h - just enough to build and
// draw a custom textured mesh through IMatRenderContext::GetDynamicMesh /
// CreateStaticMesh. Struct layouts and the IMesh vtable (multiple inheritance
// of IVertexBuffer + IIndexBuffer, leading virtual dtors) are replicated
// faithfully so vtable offsets match the engine binary. MaterialPrimitiveType_t,
// MaterialIndexFormat_t and VertexFormat_t come from IMaterialSystem.h.

#define VERTEX_MAX_TEXTURE_COORDINATES 8
#define INVALID_BUFFER_OFFSET 0xFFFFFFFFUL

class IMesh;
class CPrimList;
class CMeshBuilder;

enum VertexCompressionType_t
{
	VERTEX_COMPRESSION_INVALID = 0xFFFFFFFF,
	VERTEX_COMPRESSION_NONE = 0,
	VERTEX_COMPRESSION_ON = 1,
};

struct VertexDesc_t
{
	int	m_VertexSize_Position;
	int m_VertexSize_BoneWeight;
	int m_VertexSize_BoneMatrixIndex;
	int	m_VertexSize_Normal;
	int	m_VertexSize_Color;
	int	m_VertexSize_Specular;
	int m_VertexSize_TexCoord[VERTEX_MAX_TEXTURE_COORDINATES];
	int m_VertexSize_TangentS;
	int m_VertexSize_TangentT;
	int m_VertexSize_Wrinkle;
	int m_VertexSize_UserData;
	int m_ActualVertexSize;

	VertexCompressionType_t m_CompressionType;
	int m_NumBoneWeights;

	float*			m_pPosition;
	float*			m_pBoneWeight;
	unsigned char*	m_pBoneMatrixIndex;
	float*			m_pNormal;
	unsigned char*	m_pColor;
	unsigned char*	m_pSpecular;
	float*			m_pTexCoord[VERTEX_MAX_TEXTURE_COORDINATES];
	float*			m_pTangentS;
	float*			m_pTangentT;
	float*			m_pWrinkle;
	float*			m_pUserData;

	int	m_nFirstVertex;
	unsigned int	m_nOffset;
};

struct IndexDesc_t
{
	unsigned short*	m_pIndices;
	unsigned int	m_nOffset;
	unsigned int	m_nFirstIndex;
	unsigned char	m_nIndexSize;
};

struct MeshDesc_t : public VertexDesc_t, public IndexDesc_t
{
};

class IVertexBuffer
{
public:
	virtual ~IVertexBuffer() {}
	virtual int VertexCount() const = 0;
	virtual VertexFormat_t GetVertexFormat() const = 0;
	virtual bool IsDynamic() const = 0;
	virtual void BeginCastBuffer(VertexFormat_t format) = 0;
	virtual void EndCastBuffer() = 0;
	virtual int GetRoomRemaining() const = 0;
	virtual bool Lock(int nVertexCount, bool bAppend, VertexDesc_t& desc) = 0;
	virtual void Unlock(int nVertexCount, VertexDesc_t& desc) = 0;
	virtual void Spew(int nVertexCount, const VertexDesc_t& desc) = 0;
	virtual void ValidateData(int nVertexCount, const VertexDesc_t& desc) = 0;
};

class IIndexBuffer
{
public:
	virtual ~IIndexBuffer() {}
	virtual int IndexCount() const = 0;
	virtual MaterialIndexFormat_t IndexFormat() const = 0;
	virtual bool IsDynamic() const = 0;
	virtual void BeginCastBuffer(MaterialIndexFormat_t format) = 0;
	virtual void EndCastBuffer() = 0;
	virtual int GetRoomRemaining() const = 0;
	virtual bool Lock(int nMaxIndexCount, bool bAppend, IndexDesc_t& desc) = 0;
	virtual void Unlock(int nWrittenIndexCount, IndexDesc_t& desc) = 0;
	virtual void ModifyBegin(bool bReadOnly, int nFirstIndex, int nIndexCount, IndexDesc_t& desc) = 0;
	virtual void ModifyEnd(IndexDesc_t& desc) = 0;
	virtual void Spew(int nIndexCount, const IndexDesc_t& desc) = 0;
	virtual void ValidateData(int nIndexCount, const IndexDesc_t& desc) = 0;
};

class IMesh : public IVertexBuffer, public IIndexBuffer
{
public:
	virtual void SetPrimitiveType(MaterialPrimitiveType_t type) = 0;
	virtual void Draw(int nFirstIndex = -1, int nIndexCount = 0) = 0;
	virtual void SetColorMesh(IMesh* pColorMesh, int nVertexOffset) = 0;
	virtual void Draw(CPrimList* pLists, int nLists) = 0;
	virtual void CopyToMeshBuilder(int iStartVert, int nVerts, int iStartIndex, int nIndices, int indexOffset, CMeshBuilder& builder) = 0;
	virtual void Spew(int nVertexCount, int nIndexCount, const MeshDesc_t& desc) = 0;
	virtual void ValidateData(int nVertexCount, int nIndexCount, const MeshDesc_t& desc) = 0;
	virtual void LockMesh(int nVertexCount, int nIndexCount, MeshDesc_t& desc) = 0;
	virtual void ModifyBegin(int nFirstVertex, int nVertexCount, int nFirstIndex, int nIndexCount, MeshDesc_t& desc) = 0;
	virtual void ModifyEnd(MeshDesc_t& desc) = 0;
	virtual void UnlockMesh(int nVertexCount, int nIndexCount, MeshDesc_t& desc) = 0;
	virtual void ModifyBeginEx(bool bReadOnly, int nFirstVertex, int nVertexCount, int nFirstIndex, int nIndexCount, MeshDesc_t& desc) = 0;
	virtual void SetFlexMesh(IMesh* pMesh, int nVertexOffset) = 0;
	virtual void DisableFlexMesh() = 0;
	virtual void MarkAsDrawn() = 0;
	virtual unsigned ComputeMemoryUsed() = 0;
};
