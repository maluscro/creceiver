#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXEVENTSIZE 200

#define BUFFERSIZE 64000

char  g_buffer[ BUFFERSIZE ];
char* gp_read_begin = g_buffer; // Beginning of the buffer 'read area'
char* gp_read_end   = g_buffer; // End of the buffer 'read area'

const char* g_error_msg =
    "It was not possible to start listening to incoming connections";

char g_auxiliar_buffer[ MAXEVENTSIZE ];

char* get_event_in_buffer( )
{
  // Is there a full event in the read area of the buffer?
  char* event_end = strchr( gp_read_begin, '\n' );

  if( event_end != NULL )
  {
    // Replace the '\n' end mark with a null character, for the result to be
    // able to be handled as a string
    *event_end = '\0';

    // Return the beginning of the just found event. Update the read area of the
    // buffer, setting it just one byte after the found event.
    char* to_return = gp_read_begin;
    gp_read_begin = event_end + 1;

    return to_return;
  }
  else
  {
    return NULL;
  }
}


void receive_data( int a_socket_fd )
{
  // If the read area is already at the end of the buffer, swap  it to the
  // beggining of it.
  bool swapping_was_done = false;
  if( gp_read_end == g_buffer + BUFFERSIZE )
  {
    size_t read_area_size = gp_read_end - gp_read_begin;
    memmove( g_buffer, gp_read_begin, read_area_size );
    gp_read_begin = g_buffer;
    gp_read_end = g_buffer + read_area_size;

    swapping_was_done = true;
  }

  // Receive, after the read area. Update read area definition.
  int num_bytes_received =
      recv( a_socket_fd, gp_read_end, g_buffer + BUFFERSIZE - gp_read_end, 0 );

  if( num_bytes_received != -1 )
  {
    gp_read_end += num_bytes_received;
  }

  if( swapping_was_done )
  {
    // Clear possible already consumed information
    memset( gp_read_end, '\0', BUFFERSIZE - ( gp_read_end - gp_read_begin ) );
  }
}


char* receive_full_event( int a_socket_fd )
{
  // Is there an event already in the buffer?
  char* event_in_buffer = get_event_in_buffer( );

  // Keep receiving information until there is one
  while( event_in_buffer == NULL )
  {
    receive_data( a_socket_fd );

    event_in_buffer = get_event_in_buffer();
  }

  return event_in_buffer;
}


void receive_events( int a_socket_fd )
{
  while( 1 )
  {
    char* full_event = receive_full_event( a_socket_fd );

    printf( "An event has been received: '%s'\n", full_event );
  }
}


void* get_in_addr( const struct sockaddr* a_socket_address )
{
  // IPv4 or IPv6?
  if ( a_socket_address->sa_family == AF_INET)
  {
    return &( ( ( struct sockaddr_in* )a_socket_address )->sin_addr );
  }

  return &( ( ( struct sockaddr_in6* )a_socket_address )->sin6_addr );
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
