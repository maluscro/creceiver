#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#define __USE_XOPEN
#include <time.h>
#include <unistd.h>

#define TIMESTAMPSIZE_BODY 19
#define TIMESTAMPSIZE_EXTENSION 6
#define BUFFERSIZE 64000

/*
 * This receiver operates over a memory buffer named g_buffer, of BUFFERSIZE
 * size. Events are produced and consumed into and from the so called 'read
 * window' of the buffer. The event data streams in from a STREAM socket.
 *
 * The read window is defined by the gp_read_begin and gp_read_end pointers.
 * When a new event is requested, it is consumed from this read window (if any).
 * If there are no complete events in the window, new data is requested from the
 * socket and put at the end of the read window (i.e. the read window decreases
 * by its beginning when an event is consumed, and grows by its end when new
 * event info is retrieved from the socket).
 *
 * Note that, when data is retrieved from the socket, there is no guarantee that
 * events will be received complete.
 *
 * When the read window gets to the end of the buffer, its current contents are
 * swapped to the beginning.
 */

static char  g_buffer[ BUFFERSIZE ];
static char* gp_read_begin = g_buffer; // Beginning of the buffer 'read window'
static char* gp_read_end   = g_buffer; // End of the buffer 'read window'

static const char* g_error_msg =
    "It was not possible to start listening to incoming connections";

int timestamp_rfc3339( char* ap_output_buffer )
{
  int to_return = -1;

  // Fetch no. of seconds passed since epoch, and no. of nanoseconds into the
  // next second.
  struct timespec time_spec;
  if( clock_gettime( CLOCK_REALTIME, &time_spec ) == 0 )
  {
    // Load a tm struct with those seconds
    struct tm time;
    if( localtime_r( &time_spec.tv_sec, &time ) != NULL )
    {
      // Format the output string from the tm struct
      size_t num_chars_copied = strftime( ap_output_buffer,
                                          28,
                                          "%FT%T.",
                                          &time);

      // Add the number of microseconds into the next second
      snprintf( &( ap_output_buffer[ num_chars_copied ] ),
                28 - num_chars_copied,
                "%.6ldZ",
                ( time_spec.tv_nsec / 1000 ) );

      to_return = 0;
    }
  }

  return to_return;
}

long message_latency( const char* a_message )
{
  long to_return = -1;

  // Extract the time information from the message
  char* p_time_start = strchr( a_message, '>' );
  if( p_time_start != NULL )
  {
    // Fetch the number of seconds from epoch
    struct timespec time_spec;
    if( clock_gettime( CLOCK_REALTIME, &time_spec ) == 0 )
    {
      char time_stamp_body[ TIMESTAMPSIZE_BODY + 1 ];
      memcpy( time_stamp_body, p_time_start + 1, TIMESTAMPSIZE_BODY );
      time_stamp_body[ TIMESTAMPSIZE_BODY ] = '\0';

      struct tm time;
      strptime( time_stamp_body, "%FT%T", &time );

      long diff_seconds = time_spec.tv_sec - mktime( &time );

      char time_stamp_extension[ TIMESTAMPSIZE_EXTENSION + 1 ];
      memcpy( time_stamp_extension,
              p_time_start + 1 + TIMESTAMPSIZE_BODY + 1,
              TIMESTAMPSIZE_EXTENSION );
      time_stamp_extension[ TIMESTAMPSIZE_EXTENSION ] = '\n';

      long diff_microseconds =
          ( time_spec.tv_nsec / (long ) 1e3 ) - atol( time_stamp_extension );

      to_return = ( diff_seconds * ( long ) 1e6 ) + diff_microseconds;
    }
  }

  return to_return;
}


char* get_event_in_buffer( )
{
  // Is there a full event in the read window of the buffer?
  char* event_end = strchr( gp_read_begin, '\n' );

  if( event_end != NULL )
  {
    // Replace the '\n' end mark with a null character, for the result to be
    // able to be handled as a string
    *event_end = '\0';

    // Return the beginning of the just found event. Update the read window of
    // the buffer, setting it just one byte after the found event.
    char* to_return = gp_read_begin;
    gp_read_begin = event_end + 1;

    return to_return;
  }
  else
  {
    return NULL;
  }
}


ssize_t receive_data( int a_socket_fd )
{
  // If the read window is already at the end of the buffer, swap  it to the
  // beggining of it.
  bool swapping_was_done = false;
  if( gp_read_end == g_buffer + BUFFERSIZE )
  {
    size_t read_window_size = ( size_t ) ( gp_read_end - gp_read_begin );
    memmove( g_buffer, gp_read_begin, read_window_size );
    gp_read_begin = g_buffer;
    gp_read_end = g_buffer + read_window_size;

    swapping_was_done = true;
  }

  // Receive, after the read window. Update read window definition.
  ssize_t num_bytes_received =
      recv( a_socket_fd,
            gp_read_end,
            ( size_t )( g_buffer + BUFFERSIZE - gp_read_end ),
            0 );

  if( num_bytes_received != -1 )
  {
    gp_read_end += num_bytes_received;
  }

  if( swapping_was_done )
  {
    // Clear possible already consumed information
    memset( gp_read_end,
            '\0',
            ( size_t )( BUFFERSIZE - ( gp_read_end - gp_read_begin ) ) );
  }

  return num_bytes_received;
}


char* receive_full_event( int a_socket_fd )
{
  // Is there an event already in the buffer?
  char* event_in_buffer = get_event_in_buffer( );

  // Keep receiving information until there is one, or the connection is closed
  // by the peer
  while( event_in_buffer == NULL )
  {
    if( receive_data( a_socket_fd ) != 0 )
    {
      event_in_buffer = get_event_in_buffer();
    }
    else
    {
      break;
    }
  }

  return event_in_buffer;
}


void receive_events( int a_socket_fd )
{    
  while( 1 )
  {
    char* full_event = receive_full_event( a_socket_fd );

    if( full_event != NULL )
    {
      printf( "Latency: %ld\n", message_latency( full_event ) );
    }
    else
    {
      printf( "The connection has been closed by peer. Abandoning.\n\n");
      break;
    }
  }
}


void* get_in_addr( struct sockaddr* ap_socket_address )
{
  void* p_socket_address = ( void* ) ap_socket_address;

  // IPv4 or IPv6?
  if ( ap_socket_address->sa_family == AF_INET)
  {
    return &( ( ( struct sockaddr_in* )p_socket_address )->sin_addr );
  }

  return &( ( ( struct sockaddr_in6* )p_socket_address )->sin6_addr );
}


int create_listening_socket_from_addrinfo_list( const struct addrinfo* ap_list )
{
  int socket_fd_to_return = -1;
  int yes = 1;

  // Traverse the given items, creating sockets based on them. Bind to the
  // first one that works.
  const struct addrinfo* p_current_addrinfo = NULL;
  for( p_current_addrinfo = ap_list;
       p_current_addrinfo != NULL;
       p_current_addrinfo = p_current_addrinfo->ai_next )
  {
    socket_fd_to_return =
        socket( p_current_addrinfo->ai_family,
                p_current_addrinfo->ai_socktype,
                p_current_addrinfo->ai_protocol );

    if( socket_fd_to_return != -1 )
    {
      if ( setsockopt( socket_fd_to_return,
                       SOL_SOCKET,
                       SO_REUSEADDR,
                       &yes,
                       sizeof( int ) ) != -1 )
      {
        if( bind( socket_fd_to_return,
                  p_current_addrinfo->ai_addr,
                  p_current_addrinfo->ai_addrlen ) != -1 )
        {
          // Just abandon the loop, once the socket has been created and bound
          break;
        }
        else
        {
          close( socket_fd_to_return );
          socket_fd_to_return = -1;
        }
      }
      else
      {
        close( socket_fd_to_return );
        socket_fd_to_return = -1;
      }
    }
  }

  if( socket_fd_to_return == -1 )
  {
    printf( "%s (%s).\n", g_error_msg, "failed to create socket" );
  }

  return socket_fd_to_return;
}


int listen_to_socket( int a_socket_fd )
{
  int to_return = -1;

  // Maximum number of pending connections this receiver will hold
  int backlog = 10;

  // Just listen to the socket
  to_return = listen( a_socket_fd, backlog );

  if( to_return == -1 )
  {
    printf( "%s (failed to listen to the socket: %s).\n",
            g_error_msg,
            strerror( errno ) );
  }

  return to_return;
}


int listen_to_connection_requests( const char* a_target_name,
                                   const char* a_service_name )
{
  int socket_fd_to_return = -1;

  // Create a 'hints' struct, in order to specify which connection endtype is
  // wanted
  struct addrinfo hints;
  memset( &hints, 0, sizeof hints );
  hints.ai_family = AF_UNSPEC;     // Both IPv4 and IPv6 addresses are wanted
  hints.ai_socktype = SOCK_STREAM; // TCP socket

  // Fetch addrinfo items, from the hints above
  struct addrinfo* p_addrinfo_list = NULL;
  int error_code = getaddrinfo( a_target_name,
                                a_service_name,
                                &hints,
                                &p_addrinfo_list );

  if( error_code == 0 && p_addrinfo_list != NULL )
  {
    // Create and bind a socket
    socket_fd_to_return =
        create_listening_socket_from_addrinfo_list( p_addrinfo_list );

    // Free mem storing the addrinfo items
    freeaddrinfo( p_addrinfo_list );

    if( socket_fd_to_return != -1 )
    {
      // Listen to incoming connection requests
      bool socket_listening = ( listen_to_socket( socket_fd_to_return ) != -1 );
      if( socket_listening )
      {
        printf( "Waiting for connections...\n" );
      }
      else
      {
        close( socket_fd_to_return );
        socket_fd_to_return = -1;

        printf( "%s (failed to listen to connection requests socket: %s).\n",
                g_error_msg,
                strerror( errno ) );
      }
    }
  }
  else
  {
    printf( "%s (failed to get address information: %s).\n",
            g_error_msg,
            gai_strerror( error_code) );
  }

  return socket_fd_to_return;
}


int accept_connection( int a_socket_fd )
{
  struct sockaddr_storage incoming_conn_socket_info;
  socklen_t incoming_conn_socket_info_size = sizeof incoming_conn_socket_info;

  // Accept the first incoming connection request, when it's ready. Create a new
  // socket in order to communicate with the remote node. accept() is a blocking
  // operation!
  int new_socket_fd = accept( a_socket_fd,
                              ( struct sockaddr *) &incoming_conn_socket_info,
                              &incoming_conn_socket_info_size );

  if( new_socket_fd != -1 )
  {
    // Tell the user a connection request has been accepted
    char remote_node_address[ INET6_ADDRSTRLEN ];
    inet_ntop( incoming_conn_socket_info.ss_family,
               get_in_addr( ( struct sockaddr * )&incoming_conn_socket_info ),
               remote_node_address,
               sizeof remote_node_address );

    printf ( "A connection request coming from %s has been accepted\n",
             remote_node_address );

    // Close the requests socket, not needed anymore
    close( a_socket_fd );
  }
  else
  {
    printf( "%s (failed to create communication socket: %s).\n",
            g_error_msg,
            strerror( errno ) );
  }

  return new_socket_fd;
}


int main( int argc, char* argv[] )
{
  if( argc != 3 )
  {
    fprintf( stderr, "Usage: creceiver hostname servicename\n" );
    exit( 1 );
  }

  int to_return = 1;

  // Create socket, and use it lo listen to incoming connection requests
  int incoming_conns_socket_fd =
      listen_to_connection_requests( argv[ 1 ], argv[ 2 ]);
  if( incoming_conns_socket_fd != -1 )
  {
    // Accept the first request. Obtain a new socket, meant to be used for
    // communicating with the requesting endpoint.
    int communication_socket_fd =
        accept_connection( incoming_conns_socket_fd );
    if( communication_socket_fd != -1 )
    {
      // Receive events from the other endpoint...
      receive_events( communication_socket_fd );

      close( communication_socket_fd );

      to_return = 0;
    }

    close( incoming_conns_socket_fd );
  }

  return to_return;
}
