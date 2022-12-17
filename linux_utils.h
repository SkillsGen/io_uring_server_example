#if !RELEASE
#define Assert(Expression) if(!(Expression)) {*(int *)0 = 0;}
#else
#define Assert(Expression)
#endif

#define InvalidCodePath    Assert(!"InvalidCodePath")
#define InvalidDefaultCase default: {InvalidCodePath;} break

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef u32 b32;
typedef char utf8;

typedef float r32;
typedef double r64;
typedef intptr_t umm;

#define PI32 3.14159265358979323846
#define S32Max 2147483647
#define U16Max 65535
#define U32Max ((u32) - 1)
#define S16Max  32767
#define S16Min -32768
#define R32Max  FLT_MAX
#define R32Min -FLT_MAX

#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))
#define Kilobytes(Value) ((Value)*1024LL)
#define Megabytes(Value) (Kilobytes(Value)*1024LL)
#define Gigabytes(Value) (Megabytes(Value)*1024LL)
#define Terabytes(Value) (Gigabytes(Value)*1024LL)

inline void
DataCopy(void *Source, void *Dest, u32 Bytes)
{
    u8 *ReadCursor  = (u8 *)Source;
    u8 *WriteCursor = (u8 *)Dest;
    u8 *End = ReadCursor + Bytes;
    while(ReadCursor != End)
    {
	*WriteCursor++ = *ReadCursor++;
    }
}

struct memory_arena
{
    u64 Size;
    u64 Used;
    u8 *Base;
};

#define ZeroStruct(Instance) ZeroSize(sizeof(Instance), &(Instance))
#define ZeroArray(Count, Pointer) ZeroSize(Count*sizeof((Pointer)[0]), Pointer)
inline void
ZeroSize(umm Size, void *Ptr)
{
    u8 *Byte = (u8 *)Ptr;
    while(Size--)
    {
        *Byte++ = 0;
    }
}

#define PushStruct(Arena, Type, ...) (Type *)PushSize(Arena, sizeof(Type) __VA_OPT__(,) __VA_ARGS__)
#define PushArray(Arena, Count, Type) (Type *)PushSize(Arena, Count*sizeof(Type))

static void *
PushSize(memory_arena *Arena, u64 Size, u64 RequiredAlignment = 0)
{
    void *Result = 0;
    Assert((Arena->Used + Size) < Arena->Size);

    Result = (void *)(Arena->Base + Arena->Used);
    Arena->Used += Size;

    return(Result);
}
