#ifndef NETWORK_H
#define NETWORK_H

#include <linux/if_packet.h>  // struct sockaddr_ll
#include <stdint.h>           // types uint*_t
#include <sys/types.h>        // type ssize_t

#ifndef IP_MAXPACKET
#define IP_MAXPACKET 65535  // maximum packet size
#endif

// values of packet.direction
#define PKT_DOWN 1
#define PKT_UPL 2

// used for function parse_packet
struct packet
{
  size_t lenght;
  uint32_t local_address;
  uint32_t remote_address;
  uint16_t local_port;
  uint16_t remote_port;
  uint8_t direction;
};

// pega os dados do socket armazena no buffer e preenche a struct sockaddr_ll
// lenght tamanho maximo do buffer
ssize_t
get_packet ( struct sockaddr_ll *link_level,
             uint8_t *buffer,
             const int lenght );

// organiza os dados recebidos nas camadas 2, 3 e 4 e
// tambem verifica se é um pacote de download(entrada) ou upload(saida).
// então insere os dados na estrutura packet
int
parse_packet ( struct packet *pkt, const uint8_t *buf, struct sockaddr_ll *ll );

#endif  // NETWORK_H
