
/*
 *  Copyright (C) 2020 Mayco S. Berghetti
 *
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <arpa/inet.h>        // htons
#include <errno.h>            // variable errno
#include <linux/if_ether.h>   // struct ethhdr
#include <linux/if_packet.h>  // struct sockaddr_ll
#include <linux/ip.h>         // struct iphdr
#include <stdbool.h>          // boolean type
#include <string.h>           // strerror
#include <sys/socket.h>       // recvfrom
#include <sys/types.h>        // recvfrom

#include <stdio.h>

#include "m_error.h"
#include "network.h"
#include "sock.h"  // defined sock
#include "timer.h"

// bit dont fragment flag do cabeçalho IP
#define IP_DF 0x4000

// bit more fragments do cabeçalho IP
#define IP_MF 0x2000

// mascara para testar o offset do fragmento
#define IP_OFFMASK 0x1FFF

// máximo de pacotes IP que podem estar fragmentados simultaneamente,
// os pacotes que chegarem alem desse limite não serão calculados
// e um erro informado
#define MAX_REASSEMBLIES 32

// maximo de fragmentos que um pacote IP pode ter,
// acima disso um erro sera emitido, e os demais fragmentos desse pacote
// não serão computados
#define MAX_FRAGMENTS 64

// tempo de vida maximo de um pacote fragmentado em segundos,
// cada pacote possui um contador unico, independente da atualização
// do programa
#define LIFETIME_FRAG 1.0

// codigo de erro para numero maximo de fragmentos de um pacote aitigido
#define ER_MAX_FRAGMENTS -2

// atualiza o total de pacotes fragmentados
#define DEC_REASSEMBLE( var ) ( ( var > 0 ) ? ( var )-- : ( var = 0 ) )

// defined in main
extern bool udp;
extern uint8_t tic_tac;

// Aproveitamos do fato dos cabeçalhos TCP e UDP
// receberem as portas de origem e destino na mesma ordem,
// e como atributos iniciais, assim podemos utilizar esse estrutura
// simplificada para extrair as portas tanto de pacotes
// TCP quanto UDP, lembrando que não utilizaremos outros campos
// dos cabeçalhos.
struct tcp_udp_h
{
  uint16_t source;
  uint16_t dest;
};

// utilzado para identificar a camada de transporte (TCP, UDP)
// dos fragmentos de um pacote e tambem ter controle de tempo de vida
// do pacote e numero de fragmentos do pacote
struct pkt_ip_fragment
{
  uint16_t pkt_id;       // IP header ID value
  uint16_t source_port;  // IP header source port value
  uint16_t dest_port;    // IP header dest port value
  uint8_t c_frag;        // count of fragments, limit is MAX_FRAGMENTS
  float ttl;             // lifetime of packet
};

static int
is_first_frag ( const struct iphdr *const restrict l3,
                const struct tcp_udp_h *const restrict l4 );

static int
is_frag ( const struct iphdr *const l3 );

static void
insert_data_packet ( struct packet *pkt,
                     const uint8_t direction,
                     const uint32_t local_address,
                     const uint32_t remote_address,
                     const uint16_t local_port,
                     const uint16_t remote_port,
                     const uint32_t len );

static void
clear_frag ( void );

// armazena os dados da camada de transporte dos pacotes fragmentados
static struct pkt_ip_fragment pkt_ip_frag[MAX_REASSEMBLIES] = {0};

// contador de pacotes IP que estão fragmentados
static uint8_t count_reassemblies;

// pega os da rede e adicona em buffer
// ssize_t
// get_packet ( struct sockaddr_ll *restrict link_level,
//              uint8_t *restrict buffer,
//              const int lenght )
// {
//   socklen_t link_level_size = sizeof ( struct sockaddr_ll );
//
//   ssize_t bytes_received = recvfrom ( sock,
//                                       buffer,
//                                       lenght,
//                                       0,
//                                       ( struct sockaddr * ) link_level,
//                                       &link_level_size );
//
//   // retorna quantidade de bytes farejados
//   if ( bytes_received >= 0 && bytes_received != -1 )
//     return bytes_received;
//
//   // recvfrom retornou por conta do timeout definido no socket
//   if ( bytes_received == -1 &&
//        ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) )
//     return 0;
//
// #ifdef DEBUG
//   if ( bytes_received == -1 )
//     error ( "Error get packets %s", strerror ( errno ) );
// #endif
//
//   return -1;
// }

// separa os dados brutos em suas camadas 2, 3 4
int
parse_packet ( struct packet *pkt, struct tpacket3_hdr *ppd)//struct block_desc *pbd, const int block_num)// struct tpacket2_hdr *restrict tphdr )//,
               // const uint8_t *restrict buf),
               //const struct sockaddr_ll *restrict ll )
{
  // struct tpacket3_hdr *ppd;

  struct sockaddr_ll *ll;
  struct ethhdr *l2;
  struct iphdr *l3;
  struct tcp_udp_h *l4;

  // ppd = (struct tpacket3_hdr *) ( (uint8_t *) pbd + pbd->h1.offset_to_first_pkt);

  ll = (struct sockaddr_ll *) ((uint8_t *) ppd + TPACKET3_HDRLEN - sizeof(struct sockaddr_ll));
  l2 = (struct ethhdr *) ( (uint8_t *) ppd + ppd->tp_mac);
  l3 = (struct iphdr *)  ( (uint8_t *) ppd + ppd->tp_net);
  l4 = (struct tcp_udp_h *)  ( (uint8_t *) ppd + ppd->tp_net + (l3->ihl * 4) );
  // fprintf (stderr, "l2 proto - %d\n", ntohs(l2->h_proto));
  // fprintf (stderr, "l3 src - %d\n", l3->saddr);
  // fprintf (stderr, "l4 srcd - %d\n", ntohs(l4->dest));
  // fprintf (stderr, "ll type - %d\n", ll->sll_pkttype);
  // fprintf (stderr, "num_pkts - %d\n", pbd->h1.num_pkts);

  bool loopback = true;
  // discard traffic loopback, mac address is zero
  for ( int i = 0; i < ll->sll_halen; i++ )
    {
      if ( ll->sll_addr[i] )
        {
          loopback = false;
          break;
        }
    }

  if ( loopback )
    goto END;

  // l2 = ( struct ethhdr * ) buf;
//   l2 = (struct ethhdr *) (frame_ptr + tphdr->tp_mac);
//   // fprintf(stderr, "l2->h_proto - %d\n", ntohs(l2->h_proto));
//
  // not is a packet internet protocol
  if ( ntohs ( l2->h_proto ) != ETH_P_IP )
    goto END;


//
//   // l3 = ( struct iphdr * ) ( buf + ETH_HLEN );
//   l3 = (struct iphdr *) (frame_ptr + tphdr->tp_net);
//   l4 = (struct tcp_udp_h *) (frame_ptr + tphdr->tp_net + (l3->ihl * 4));
//     // fprintf(stderr, "l3->h_proto - %d\n", l3->protocol);
//     // fprintf(stderr, "nt packet - rem_port = %d\n", ntohs(l4->dest));
//
  // se não for um protocolo suportado
  if (l3->protocol != IPPROTO_TCP && l3->protocol != IPPROTO_UDP)
    goto END;
  // caso tenha farejado pacotes TCP e opção udp estaja ligada, falha
  else if ( l3->protocol == IPPROTO_TCP && udp )
    goto END;
  // caso tenha farejado pacote UDP e a opção udp esteja desligada, falha
  else if ( l3->protocol == IPPROTO_UDP && !udp )
    goto END;
  // pacote não suportado
  // fprintf (stderr, "aqui\n");



//
  // atigido MAX_REASSEMBLIES, dados não computados
  if ( is_first_frag ( l3, l4 ) == -1 )
    goto END;
//
  int id = is_frag ( l3 );
  // create packet
  if ( ll->sll_pkttype == PACKET_OUTGOING )
    {  // upload
      if ( id == -1 )
        // não é um fragmento, assumi que isso é maioria dos casos
        insert_data_packet ( pkt,
                             PKT_UPL,
                             l3->saddr,
                             l3->daddr,
                             ntohs ( l4->source ),
                             ntohs ( l4->dest ),
                             ppd->tp_snaplen );

      else if ( id >= 0 )
        // é um fragmento, pega dados da camada de transporte
        // no array de pacotes fragmentados
        insert_data_packet ( pkt,
                             PKT_UPL,
                             l3->saddr,
                             l3->daddr,
                             pkt_ip_frag[id].source_port,
                             pkt_ip_frag[id].dest_port,
                             ppd->tp_snaplen );
      else
        // é um fragmento, porem maximo de fragmentos de um pacote atingido.
        // dados não serão computados
        goto END;

      return 1;
    }
  else
    {  // download
      if ( id == -1 )
        // não é um fragmento, assumi que isso é maioria dos casos
        insert_data_packet ( pkt,
                             PKT_DOWN,
                             l3->daddr,
                             l3->saddr,
                             ntohs ( l4->dest ),
                             ntohs ( l4->source ),
                             ppd->tp_snaplen);
      else if ( id >= 0 )
        // é um fragmento, pega dados da camada de transporte
        // no array de pacotes fragmentados
        insert_data_packet ( pkt,
                             PKT_DOWN,
                             l3->daddr,
                             l3->saddr,
                             pkt_ip_frag[id].dest_port,
                             pkt_ip_frag[id].source_port,
                             ppd->tp_snaplen );
      else
        // é um fragmento, porem maximo de fragmentos de um pacote atingido.
        // dados não serão computados
        goto END;

      return 1;
    }

// caso exista pacotes que foram fragmentados,
// faz checagem e descarta pacotes que ainda não enviaram todos
// os fragmentos no tempo limite de LIFETIME_FRAG segundos
END:
  if ( count_reassemblies )
    clear_frag ();

  return 0;
}

// Testa se o bit more fragments (MF) esta ligado
// e se o offset é 0, caso sim esse é o primeiro fragmento,
// então ocupa um espaço na estrutura pkt_ip_frag com os dados
// da camada de transporte do pacote, para associar aos demais
// fragmentos posteriormente.
// retorna 1 para primeiro fragmento
// retorna 0 se não for primeiro fragmento
// retorna -1 caso de erro, buffer cheio
static int
is_first_frag ( const struct iphdr *const restrict l3,
                const struct tcp_udp_h *const restrict l4 )
{
  // bit não fragmente ligado, logo não pode ser um fragmento
  if ( ntohs ( l3->frag_off ) & IP_DF )
    return 0;

  // bit MF ligado e offset igual a 0,
  // indica que é o primeiro fragmento
  if ( ( ntohs ( l3->frag_off ) & IP_MF ) &&
       ( ( ntohs ( l3->frag_off ) & IP_OFFMASK ) == 0 ) )
    {
      if ( ++count_reassemblies > MAX_REASSEMBLIES )
        {
#ifdef DEBUG
          error ( "Maximum number of %d fragmented packets reached, "
                  "packets surpluses are not calculated.",
                  MAX_REASSEMBLIES );
#endif

          count_reassemblies = MAX_REASSEMBLIES;
          return -1;
        }

      // busca posição livre para adicionar dados do pacote fragmentado
      for ( size_t i = 0; i < MAX_REASSEMBLIES; i++ )
        {
          if ( pkt_ip_frag[i].ttl == 0 )  // posição livre no array
            {
              pkt_ip_frag[i].pkt_id = l3->id;
              pkt_ip_frag[i].source_port = ntohs ( l4->source );
              pkt_ip_frag[i].dest_port = ntohs ( l4->dest );
              pkt_ip_frag[i].c_frag = 1;            // first fragment
              pkt_ip_frag[i].ttl = start_timer ();  // anota tempo atual

              // it's first fragment
              return 1;
            }
        }
    }

  // it's not first fragment
  return 0;
}

// verifica se é um fragmento
// return -1 para indicar que não,
// return -2, maximo de fragmentos de um pacote atingido
// qualquer outro valor maior igual a 0 indica que sim,
// sendo o valor o indice correspondente no array de fragmentos
static int
is_frag ( const struct iphdr *const l3 )
{
  // bit não fragmentação ligado, logo não pode ser um fragmento
  if ( ntohs ( l3->frag_off ) & IP_DF )
    return -1;

  // se o deslocamento for maior que 0, indica que é um fragmento
  if ( ntohs ( l3->frag_off ) & IP_OFFMASK )
    {
      // percorre todo array de pacotes fragmentados...
      for ( size_t i = 0; i < MAX_REASSEMBLIES; i++ )
        {
          // ... e procura o fragmento com base no campo id do cabeçalho IP
          if ( pkt_ip_frag[i].pkt_id == l3->id )
            {
              // se o total de fragmentos de um pacote for atingido
              if ( ++pkt_ip_frag[i].c_frag > MAX_FRAGMENTS )
                {
#ifdef DEBUG
                  error ( "Maximum number of %d fragments in a "
                          "package reached",
                          MAX_FRAGMENTS );
#endif
                  pkt_ip_frag[i].c_frag = MAX_FRAGMENTS;
                  return ER_MAX_FRAGMENTS;
                }

              // se for o  ultimo fragmento
              // libera a posição do array de fragmentos
              if ( ( ntohs ( l3->frag_off ) & IP_MF ) == 0 )
                {
                  pkt_ip_frag[i].ttl = 0;
                  DEC_REASSEMBLE ( count_reassemblies );
                }

              // fragmento localizado, retorna o indice do array de fragmentos
              return i;
            }
        }
    }

  // it's not a fragment
  return -1;
}

// Remove do array de pacotes fragmentados pacotes que
// ja tenham atingido o tempo limite de LIFETIME_FRAG definido em ttl e ainda
// tenham mais fragmentos para enviar.
// Essa ação é necessaria caso alguma aplicação numca envie um fragmento
// sinalizando ser o ultimo, os fragmentos subsequentes enviados
// por esse pacote apos o tempo limite, não serão calculados
// nas estatisticas de rede do processo
static void
clear_frag ( void )
{
  for ( size_t i = 0; i < count_reassemblies; i++ )
    {
      if ( timer ( pkt_ip_frag[i].ttl ) >= LIFETIME_FRAG )
        {
          pkt_ip_frag[i].ttl = 0;
          DEC_REASSEMBLE ( count_reassemblies );
        }
    }
}

static inline void
insert_data_packet ( struct packet *pkt,
                     const uint8_t direction,
                     const uint32_t local_address,
                     const uint32_t remote_address,
                     const uint16_t local_port,
                     const uint16_t remote_port,
                     const uint32_t len)
{
  pkt->direction = direction;
  pkt->local_address = local_address;
  pkt->remote_address = remote_address;
  pkt->local_port = local_port;
  pkt->remote_port = remote_port;
  pkt->lenght = len;
}
