/*****************************************************************************
 * udp.c
 *****************************************************************************
 * Copyright (C) 2001, 2002 VideoLAN
 * $Id: udp.c,v 1.12 2003/08/01 19:38:48 fenrir Exp $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Eric Petit <titer@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <vlc/vlc.h>
#include <vlc/input.h>
#include <vlc/sout.h>

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif

#ifdef WIN32
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   ifndef IN_MULTICAST
#       define IN_MULTICAST(a) IN_CLASSD(a)
#   endif
#else
#   include <sys/socket.h>
#endif

#include "network.h"

#define DEFAULT_PORT 1234
#define LATENCY     100000
#define MAX_ERROR    500000
/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/
static int     Open   ( vlc_object_t * );
static void    Close  ( vlc_object_t * );

static int     Write( sout_access_out_t *, sout_buffer_t * );
static int     Seek ( sout_access_out_t *, off_t  );

static void    ThreadWrite( vlc_object_t * );

static sout_buffer_t *NewUDPPacket( sout_access_out_t *, mtime_t );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define CACHING_TEXT N_("caching value in ms")
#define CACHING_LONGTEXT N_( \
    "Allows you to modify the default caching value for udp streams. This " \
    "value should be set in miliseconds units." )

vlc_module_begin();
    set_description( _("UDP stream ouput") );
    add_category_hint( N_("udp stream output"), NULL , VLC_TRUE );
    add_integer( "udp-sout-caching", DEFAULT_PTS_DELAY / 1000, NULL, CACHING_TEXT, CACHING_LONGTEXT, VLC_TRUE );
    set_capability( "sout access", 100 );
    add_shortcut( "udp" );
    add_shortcut( "rtp" ); // Will work only with ts muxer
    set_callbacks( Open, Close );
vlc_module_end();

typedef struct sout_access_thread_s
{
    VLC_COMMON_MEMBERS

    sout_instance_t *p_sout;

    sout_fifo_t *p_fifo;

    int         i_handle;

} sout_access_thread_t;

struct sout_access_out_sys_t
{
    int                 b_rtpts;  // 1 if add rtp/ts header
    uint16_t            i_sequence_number;
    uint32_t            i_ssrc;

    unsigned int        i_mtu;

    sout_buffer_t       *p_buffer;

    sout_access_thread_t *p_thread;

};

/*****************************************************************************
 * Open: open the file
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    sout_access_out_t       *p_access = (sout_access_out_t*)p_this;
    sout_access_out_sys_t   *p_sys;

    char                *psz_parser;
    char                *psz_dst_addr;
    int                 i_dst_port;

    module_t            *p_network;
    network_socket_t    socket_desc;

    char                *val;

    if( !( p_sys = p_access->p_sys =
                malloc( sizeof( sout_access_out_sys_t ) ) ) )
    {
        msg_Err( p_access, "Not enough memory" );
        return( VLC_EGENERIC );
    }


    if( p_access->psz_access != NULL &&
        !strcmp( p_access->psz_access, "rtp" ) )
    {
        msg_Warn( p_access, "becarefull that rtp ouput work only with ts "
                  "payload(not an error)" );
        p_sys->b_rtpts = 1;
    }
    else
    {
        p_sys->b_rtpts = 0;
    }

    psz_parser = strdup( p_access->psz_name );

    psz_dst_addr = psz_parser;
    i_dst_port = 0;

    if ( *psz_parser == '[' )
    {
        while( *psz_parser && *psz_parser != ']' )
        {
            psz_parser++;
        }
    }
    while( *psz_parser && *psz_parser != ':' )
    {
        psz_parser++;
    }
    if( *psz_parser == ':' )
    {
        *psz_parser = '\0';
        psz_parser++;
        i_dst_port = atoi( psz_parser );
    }
    if( i_dst_port <= 0 )
    {
        i_dst_port = DEFAULT_PORT;
    }

    p_sys->p_thread =
        vlc_object_create( p_access, sizeof( sout_access_thread_t ) );
    if( !p_sys->p_thread )
    {
        msg_Err( p_access, "out of memory" );
        return( VLC_EGENERIC );
    }

    p_sys->p_thread->p_sout = p_access->p_sout;
    p_sys->p_thread->b_die  = 0;
    p_sys->p_thread->b_error= 0;
    p_sys->p_thread->p_fifo = sout_FifoCreate( p_access->p_sout );

    socket_desc.i_type = NETWORK_UDP;
    socket_desc.psz_server_addr = psz_dst_addr;
    socket_desc.i_server_port   = i_dst_port;
    socket_desc.psz_bind_addr   = "";
    socket_desc.i_bind_port     = 0;
    socket_desc.i_ttl           = 0;
    if( ( val = sout_cfg_find_value( p_access->p_cfg, "ttl" ) ) )
    {
        socket_desc.i_ttl = atoi( val );
    }
    p_sys->p_thread->p_private = (void*)&socket_desc;
    if( !( p_network = module_Need( p_sys->p_thread,
                                    "network", "" ) ) )
    {
        msg_Err( p_access, "failed to open a connection (udp)" );
        return( VLC_EGENERIC );
    }
    module_Unneed( p_sys->p_thread, p_network );

    p_sys->p_thread->i_handle = socket_desc.i_handle;
    p_sys->i_mtu     = socket_desc.i_mtu;

    if( vlc_thread_create( p_sys->p_thread, "sout write thread", ThreadWrite,
                           VLC_THREAD_PRIORITY_OUTPUT, VLC_FALSE ) )
    {
        msg_Err( p_access->p_sout, "cannot spawn sout access thread" );
        vlc_object_destroy( p_sys->p_thread );
        return( VLC_EGENERIC );
    }

    srand( (uint32_t)mdate());
    p_sys->p_buffer          = NULL;
    p_sys->i_sequence_number = rand()&0xffff;
    p_sys->i_ssrc            = rand()&0xffffffff;

    p_access->pf_write       = Write;
    p_access->pf_seek        = Seek;

    msg_Info( p_access, "Open: addr:`%s' port:`%d'",
              psz_dst_addr, i_dst_port );

    free( psz_dst_addr );
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: close the target
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    sout_access_out_t       *p_access = (sout_access_out_t*)p_this;
    sout_access_out_sys_t   *p_sys = p_access->p_sys;
    int                 i;

    p_sys->p_thread->b_die = 1;
    for( i = 0; i < 10; i++ )
    {
        sout_buffer_t       *p_dummy;

        p_dummy = sout_BufferNew( p_access->p_sout, p_sys->i_mtu );
        p_dummy->i_dts = 0;
        p_dummy->i_pts = 0;
        p_dummy->i_length = 0;
        sout_FifoPut( p_sys->p_thread->p_fifo, p_dummy );
    }
    vlc_thread_join( p_sys->p_thread );

    sout_FifoDestroy( p_access->p_sout, p_sys->p_thread->p_fifo );

    if( p_sys->p_buffer )
    {
        sout_BufferDelete( p_access->p_sout, p_sys->p_buffer );
    }

#if defined( UNDER_CE )
    CloseHandle( (HANDLE)p_sys->p_thread->i_handle );
#elif defined( WIN32 )
    closesocket( p_sys->p_thread->i_handle );
#else
    close( p_sys->p_thread->i_handle );
#endif

    free( p_sys );
    msg_Info( p_access, "Close" );
}

/*****************************************************************************
 * Read: standard read on a file descriptor.
 *****************************************************************************/
static int Write( sout_access_out_t *p_access, sout_buffer_t *p_buffer )
{
    sout_access_out_sys_t   *p_sys = p_access->p_sys;
    unsigned int i_write;

    while( p_buffer )
    {
        sout_buffer_t *p_next;
        if( p_buffer->i_size > p_sys->i_mtu )
        {
            msg_Warn( p_access, "arggggggggggggg packet size > mtu" );
            i_write = p_sys->i_mtu;
        }
        else
        {
            i_write = p_buffer->i_size;
        }

        /* if we have enough data, enque the buffer */
        if( p_sys->p_buffer &&
            p_sys->p_buffer->i_size + i_write > p_sys->i_mtu )
        {
            sout_FifoPut( p_sys->p_thread->p_fifo, p_sys->p_buffer );
            p_sys->p_buffer = NULL;
        }

        if( !p_sys->p_buffer )
        {
            p_sys->p_buffer = NewUDPPacket( p_access, p_buffer->i_dts );
        }

        if( p_buffer->i_size > 0 )
        {
            memcpy( p_sys->p_buffer->p_buffer + p_sys->p_buffer->i_size,
                    p_buffer->p_buffer, i_write );
            p_sys->p_buffer->i_size += i_write;
        }
        p_next = p_buffer->p_next;
        sout_BufferDelete( p_access->p_sout, p_buffer );
        p_buffer = p_next;
    }

    return( p_sys->p_thread->b_error ? -1 : 0 );
}

/*****************************************************************************
 * Seek: seek to a specific location in a file
 *****************************************************************************/
static int Seek( sout_access_out_t *p_access, off_t i_pos )
{

    msg_Err( p_access, "udp sout access cannot seek" );
    return( -1 );
}

/*****************************************************************************
 * NewUDPPacket: allocate a new UDP packet of size p_sys->i_mtu
 *****************************************************************************/
static sout_buffer_t *NewUDPPacket( sout_access_out_t *p_access, mtime_t i_dts)
{
    sout_access_out_sys_t *p_sys = p_access->p_sys;
    sout_buffer_t *p_buffer;

    p_buffer = sout_BufferNew( p_access->p_sout, p_sys->i_mtu );
    p_buffer->i_dts = i_dts;
    p_buffer->i_size = 0;

    if( p_sys->b_rtpts )
    {
        mtime_t i_timestamp = p_buffer->i_dts * 9 / 100;

        /* add rtp/ts header */
        p_buffer->p_buffer[0] = 0x80;
        p_buffer->p_buffer[1] = 0x21; // mpeg2-ts

        p_buffer->p_buffer[2] = ( p_sys->i_sequence_number >> 8 )&0xff;
        p_buffer->p_buffer[3] = p_sys->i_sequence_number&0xff;
        p_sys->i_sequence_number++;

        p_buffer->p_buffer[4] = ( i_timestamp >> 24 )&0xff;
        p_buffer->p_buffer[5] = ( i_timestamp >> 16 )&0xff;
        p_buffer->p_buffer[6] = ( i_timestamp >>  8 )&0xff;
        p_buffer->p_buffer[7] = i_timestamp&0xff;

        p_buffer->p_buffer[ 8] = ( p_sys->i_ssrc >> 24 )&0xff;
        p_buffer->p_buffer[ 9] = ( p_sys->i_ssrc >> 16 )&0xff;
        p_buffer->p_buffer[10] = ( p_sys->i_ssrc >>  8 )&0xff;
        p_buffer->p_buffer[11] = p_sys->i_ssrc&0xff;

        p_buffer->i_size = 12;
    }

    return p_buffer;
}

/*****************************************************************************
 * ThreadWrite: Write a packet on the network at the good time.
 *****************************************************************************/
static void ThreadWrite( vlc_object_t *p_this )
{
    sout_access_thread_t *p_thread = (sout_access_thread_t*)p_this;
    sout_instance_t      *p_sout = p_thread->p_sout;
    mtime_t              i_pts_delay;
    mtime_t              i_date_last = -1;

    /* Get the i_pts_delay value */
    i_pts_delay = config_GetInt( p_this, "udp-sout-caching" ) * 1000;

    while( ! p_thread->b_die )
    {
        sout_buffer_t *p_pk;
        mtime_t       i_date;

        p_pk = sout_FifoGet( p_thread->p_fifo );

        i_date = i_pts_delay + p_pk->i_dts;
        if( i_date_last > 0 )
        {
            if( i_date - i_date_last > 2000000 )
            {
                msg_Dbg( p_thread, "mmh, hole > 2s -> drop" );

                sout_BufferDelete( p_sout, p_pk );
                i_date_last = i_date;
                continue;
            }
            else if( i_date - i_date_last < 0 )
            {
                msg_Dbg( p_thread, "mmh, paquets in the past -> drop" );

                sout_BufferDelete( p_sout, p_pk );
                i_date_last = i_date;
                continue;
            }
        }


        mwait( i_date );
        send( p_thread->i_handle, p_pk->p_buffer, p_pk->i_size, 0 );
        sout_BufferDelete( p_sout, p_pk );
        i_date_last = i_date;
    }
}
