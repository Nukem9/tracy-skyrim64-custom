#ifndef __TRACYD3D11_HPP__
#define __TRACYD3D11_HPP__

// Include this file after you include DirectX 11 headers.

#if !defined TRACY_ENABLE || !defined _WIN32

#define TracyDx11Context(x,y)
#define TracyDx11NamedZone(x,y,z)
#define TracyDx11NamedZoneC(x,y,z,w)
#define TracyDx11Zone(x,y)
#define TracyDx11ZoneC(x,y,z)
#define TracyDx11Collect(x)

#define TracyDx11NamedZoneS(x,y,z,w)
#define TracyDx11NamedZoneCS(x,y,z,w,v)
#define TracyDx11ZoneS(x,y,z)
#define TracyDx11ZoneCS(x,y,z,w)

#else

#include <atomic>
#include <assert.h>
#include <stdlib.h>

#include "Tracy.hpp"
#include "client/TracyProfiler.hpp"
#include "client/TracyCallstack.hpp"
#include "common/TracyAlign.hpp"
#include "common/TracyAlloc.hpp"

#define TracyDx11Context( device, devicectx ) tracy::s_dx11Ctx.ptr = (tracy::Dx11Ctx*)tracy::tracy_malloc( sizeof( tracy::Dx11Ctx ) ); new(tracy::s_dx11Ctx.ptr) tracy::Dx11Ctx( device, devicectx );
#define TracyDx11NamedZone( varname, devicectx, name ) static const tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, 0 }; tracy::Dx11CtxScope varname( &TracyConcat(__tracy_gpu_source_location,__LINE__), devicectx );
#define TracyDx11NamedZoneC( varname, devicectx, name, color ) static const tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, color }; tracy::Dx11CtxScope varname( &TracyConcat(__tracy_gpu_source_location,__LINE__), devicectx );
#define TracyDx11Zone( devicectx, name ) TracyDx11NamedZone( ___tracy_gpu_zone, devicectx, name )
#define TracyDx11ZoneC( devicectx, name, color ) TracyDx11NamedZoneC( ___tracy_gpu_zone, devicectx, name, color )
#define TracyDx11Collect( devicectx ) tracy::s_dx11Ctx.ptr->Collect( devicectx );

#ifdef TRACY_HAS_CALLSTACK
#  define TracyDx11NamedZoneS( varname, devicectx, name, depth ) static const tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, 0 }; tracy::Dx11CtxScope varname( &TracyConcat(__tracy_gpu_source_location,__LINE__), devicectx, depth );
#  define TracyDx11NamedZoneCS( varname, devicectx, name, color, depth ) static const tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,__LINE__) { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, color }; tracy::Dx11CtxScope varname( &TracyConcat(__tracy_gpu_source_location,__LINE__), devicectx, depth );
#  define TracyDx11ZoneS( devicectx, name, depth ) TracyDx11NamedZoneS( ___tracy_gpu_zone, devicectx, name, depth )
#  define TracyDx11ZoneCS( devicectx, name, color, depth ) TracyDx11NamedZoneCS( ___tracy_gpu_zone, devicectx, name, color, depth )
#else
#  define TracyDx11NamedZoneS( varname, devicectx, name, depth ) TracyDx11NamedZone( varname, devicectx, name )
#  define TracyDx11NamedZoneCS( varname, devicectx, name, color, depth ) TracyDx11NamedZoneC( varname, devicectx, name, color )
#  define TracyDx11ZoneS( devicectx, name, depth ) TracyDx11Zone( devicectx, name )
#  define TracyDx11ZoneCS( devicectx, name, color, depth ) TracyDx11ZoneC( name, color )
#endif

namespace tracy
{

extern std::atomic<uint8_t> s_gpuCtxCounter;

class Dx11Ctx
{
    friend class Dx11CtxScope;

    enum { QueryCount = 64 * 1024 };

public:
    Dx11Ctx(ID3D11Device* device, ID3D11DeviceContext* devicectx)
        : m_context( s_gpuCtxCounter.fetch_add( 1, std::memory_order_relaxed ) )
        , m_head( 0 )
        , m_tail( 0 )
    {
        assert( m_context != 255 );

        for ( int i = 0; i < QueryCount; i++ )
        {
            HRESULT hr = S_OK;
            D3D11_QUERY_DESC desc;
            desc.MiscFlags = 0;

            desc.Query = D3D11_QUERY_TIMESTAMP;
            hr |= device->CreateQuery( &desc, &m_queries[i] );

            desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            hr |= device->CreateQuery( &desc, &m_disjoints[i] );

            m_disjointMap[i] = nullptr;

            assert( SUCCEEDED( hr ) );
        }

        // Force query the initial GPU timestamp (pipeline stall)
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        UINT64 timestamp;
        for ( int attempts = 0; attempts < 50; attempts++ )
        {
            devicectx->Begin( m_disjoints[0] );
            devicectx->End( m_queries[0] );
            devicectx->End( m_disjoints[0] );
            devicectx->Flush();

            while ( devicectx->GetData( m_disjoints[0], &disjoint, sizeof( disjoint ), 0 ) == S_FALSE )
                /* Nothing */;

            if ( disjoint.Disjoint )
                continue;

            while ( devicectx->GetData( m_queries[0], &timestamp, sizeof( timestamp ), 0 ) == S_FALSE )
                /* Nothing */;

            break;
        }

        int64_t tgpu = timestamp * ( 1000000000ull / disjoint.Frequency );
        int64_t tcpu = Profiler::GetTime();

        const float period = 1.f;
        Magic magic;
        auto& token = s_token.ptr;
        auto& tail = token->get_tail_index();
        auto item = token->enqueue_begin<tracy::moodycamel::CanAlloc>( magic );
        MemWrite( &item->hdr.type, QueueType::GpuNewContext );
        MemWrite( &item->gpuNewContext.cpuTime, tcpu );
        MemWrite( &item->gpuNewContext.gpuTime, tgpu );
        MemWrite( &item->gpuNewContext.thread, GetThreadHandle() );
        MemWrite( &item->gpuNewContext.period, period );
        MemWrite( &item->gpuNewContext.context, m_context );
        MemWrite( &item->gpuNewContext.accuracyBits, uint8_t( 0 ) );

#ifdef TRACY_ON_DEMAND
        s_profiler.DeferItem( *item );
#endif

        tail.store( magic + 1, std::memory_order_release );
    }

    ~Dx11Ctx()
    {
        for ( int i = 0; i < QueryCount; i++ )
        {
            m_queries[i]->Release();
            m_disjoints[i]->Release();
            m_disjointMap[i] = nullptr;
        }
    }

    void Collect(ID3D11DeviceContext* devicectx)
    {
        ZoneScopedC( Color::Red4 );

        if( m_tail == m_head ) return;

#ifdef TRACY_ON_DEMAND
        if( !s_profiler.IsConnected() )
        {
            m_head = m_tail = 0;
            return;
        }
#endif

        auto start = m_tail;
        auto end = m_head + QueryCount;
        auto cnt = ( end - start ) % QueryCount;
        while ( cnt > 1 )
        {
            auto mid = start + cnt / 2;

            bool available =
                devicectx->GetData( m_disjointMap[mid % QueryCount], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH ) == S_OK &&
                devicectx->GetData( m_queries[mid % QueryCount], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH ) == S_OK;

            if ( available )
            {
                start = mid;
            }
            else
            {
                end = mid;
            }
            cnt = ( end - start ) % QueryCount;
        }

        start %= QueryCount;

        Magic magic;
        auto& token = s_token.ptr;
        auto& tail = token->get_tail_index();

        while ( m_tail != start )
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
            UINT64 time;

            devicectx->GetData( m_disjointMap[m_tail], &disjoint, sizeof(disjoint), 0 );
            devicectx->GetData( m_queries[m_tail], &time, sizeof(time), 0 );

            time *= ( 1000000000ull / disjoint.Frequency );

            auto item = token->enqueue_begin<tracy::moodycamel::CanAlloc>( magic );
            MemWrite( &item->hdr.type, QueueType::GpuTime );
            MemWrite( &item->gpuTime.gpuTime, (int64_t)time );
            MemWrite( &item->gpuTime.queryId, (uint16_t)m_tail );
            MemWrite( &item->gpuTime.context, m_context );
            tail.store( magic + 1, std::memory_order_release );
            m_tail = ( m_tail + 1 ) % QueryCount;
        }
    }

private:
    tracy_force_inline unsigned int NextQueryId()
    {
        const auto id = m_head;
        m_head = ( m_head + 1 ) % QueryCount;
        assert( m_head != m_tail );
        return id;
    }

    tracy_force_inline ID3D11Query *TranslateQueryId( unsigned int id )
    {
        return m_queries[id];
    }

    tracy_force_inline ID3D11Query *MapDisjointQueryId( unsigned int id, unsigned int disjointId )
    {
        m_disjointMap[id] = m_disjoints[disjointId];
        return m_disjoints[disjointId];
    }

    tracy_force_inline uint8_t GetId() const
    {
        return m_context;
    }

    ID3D11Query *m_queries[QueryCount];
    ID3D11Query *m_disjoints[QueryCount];
    ID3D11Query *m_disjointMap[QueryCount]; // Multiple time queries can have one disjoint query
    uint8_t m_context;

    unsigned int m_head;
    unsigned int m_tail;
};

extern Dx11CtxWrapper s_dx11Ctx;

class Dx11CtxScope
{
public:
    tracy_force_inline Dx11CtxScope( const SourceLocationData* srcloc, ID3D11DeviceContext* context )
        : m_context( context )
        , m_disjointId( 0 )
#ifdef TRACY_ON_DEMAND
        , m_active( s_profiler.IsConnected() )
#endif
    {
#ifdef TRACY_ON_DEMAND
        if( !m_active ) return;
#endif
        const auto queryId = s_dx11Ctx.ptr->NextQueryId();
        m_context->Begin( s_dx11Ctx.ptr->MapDisjointQueryId( queryId, queryId ) );
        m_context->End( s_dx11Ctx.ptr->TranslateQueryId( queryId ) );

        m_disjointId = queryId;

        Magic magic;
        auto& token = s_token.ptr;
        auto& tail = token->get_tail_index();
        auto item = token->enqueue_begin<tracy::moodycamel::CanAlloc>( magic );
        MemWrite( &item->hdr.type, QueueType::GpuZoneBegin );
        MemWrite( &item->gpuZoneBegin.cpuTime, Profiler::GetTime() );
        MemWrite( &item->gpuZoneBegin.srcloc, (uint64_t)srcloc );
        memset( &item->gpuZoneBegin.thread, 0, sizeof( item->gpuZoneBegin.thread ) );
        MemWrite( &item->gpuZoneBegin.queryId, uint16_t( queryId ) );
        MemWrite( &item->gpuZoneBegin.context, s_dx11Ctx.ptr->GetId() );
        tail.store( magic + 1, std::memory_order_release );
    }

    tracy_force_inline Dx11CtxScope( const SourceLocationData* srcloc, ID3D11DeviceContext* context, int depth )
        : m_context( context )
#ifdef TRACY_ON_DEMAND
        , m_active( s_profiler.IsConnected() )
#endif
    {
#ifdef TRACY_ON_DEMAND
        if( !m_active ) return;
#endif
        const auto queryId = s_dx11Ctx.ptr->NextQueryId();
        m_context->Begin( s_dx11Ctx.ptr->MapDisjointQueryId( queryId, queryId ) );
        m_context->End( s_dx11Ctx.ptr->TranslateQueryId( queryId ) );

        m_disjointId = queryId;
        const auto thread = GetThreadHandle();

        Magic magic;
        auto& token = s_token.ptr;
        auto& tail = token->get_tail_index();
        auto item = token->enqueue_begin<tracy::moodycamel::CanAlloc>( magic );
        MemWrite( &item->hdr.type, QueueType::GpuZoneBeginCallstack );
        MemWrite( &item->gpuZoneBegin.cpuTime, Profiler::GetTime() );
        MemWrite( &item->gpuZoneBegin.srcloc, (uint64_t)srcloc );
        MemWrite( &item->gpuZoneBegin.thread, thread );
        MemWrite( &item->gpuZoneBegin.queryId, uint16_t( queryId ) );
        MemWrite( &item->gpuZoneBegin.context, s_dx11Ctx.ptr->GetId() );
        tail.store( magic + 1, std::memory_order_release );

        s_profiler.SendCallstack( depth, thread );
    }

    tracy_force_inline ~Dx11CtxScope()
    {
#ifdef TRACY_ON_DEMAND
        if( !m_active ) return;
#endif
        const auto queryId = s_dx11Ctx.ptr->NextQueryId();
        m_context->End( s_dx11Ctx.ptr->TranslateQueryId( queryId ) );
        m_context->End( s_dx11Ctx.ptr->MapDisjointQueryId( queryId, m_disjointId ) );

        Magic magic;
        auto& token = s_token.ptr;
        auto& tail = token->get_tail_index();
        auto item = token->enqueue_begin<tracy::moodycamel::CanAlloc>( magic );
        MemWrite( &item->hdr.type, QueueType::GpuZoneEnd );
        MemWrite( &item->gpuZoneEnd.cpuTime, Profiler::GetTime() );
        MemWrite( &item->gpuZoneEnd.queryId, uint16_t( queryId ) );
        MemWrite( &item->gpuZoneEnd.context, s_dx11Ctx.ptr->GetId() );
        tail.store( magic + 1, std::memory_order_release );
    }

private:
    ID3D11DeviceContext *m_context;
    unsigned int m_disjointId;

#ifdef TRACY_ON_DEMAND
    const bool m_active;
#endif
};

}

#endif

#endif
