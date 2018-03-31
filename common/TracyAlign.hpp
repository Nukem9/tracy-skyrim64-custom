#ifndef __TRACYALIGN_HPP__
#define __TRACYALIGN_HPP__

#include <string.h>

#include "TracyForceInline.hpp"

namespace tracy
{

template<typename T>
tracy_force_inline T MemRead( T* ptr )
{
    T val;
    memcpy( &val, ptr, sizeof( T ) );
    return val;
}

template<typename T>
tracy_force_inline void MemWrite( T* ptr, T val )
{
    memcpy( ptr, &val, sizeof( T ) );
}

}

#endif