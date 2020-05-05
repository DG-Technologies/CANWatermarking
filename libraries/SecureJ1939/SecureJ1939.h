#include <FlexCAN_T4.h>
#include <AES.h>
#include <OMAC.h>
#include <EEPROM.h>

FlexCAN_T4<CAN1, RX_SIZE_256, TX_SIZE_1024> vehicle_can;
FlexCAN_T4<CAN2, RX_SIZE_256, TX_SIZE_1024> ecu_can;

#define CMAC_BLOCK_SIZE             512
#define AES_BLOCK_SIZE              16
#define NUM_SOURCE_ADDRESSES        24 
#define NUM_DESTINATION_ADDRESSES   3 // Global and Self

char serial_string[19];
char model_string[14];
  
// Create an array to store the source addresses
uint8_t own_public_key[64];
uint8_t source_addresses[NUM_SOURCE_ADDRESSES];
uint8_t encrypted_session_key[NUM_SOURCE_ADDRESSES][16];
uint8_t device_public_key[NUM_SOURCE_ADDRESSES][64];
bool need_device_public_key[NUM_SOURCE_ADDRESSES];
uint8_t cmac_data[NUM_SOURCE_ADDRESSES][CMAC_BLOCK_SIZE];
uint8_t cmac_keys[NUM_SOURCE_ADDRESSES][AES_BLOCK_SIZE];
uint8_t omac[NUM_SOURCE_ADDRESSES][AES_BLOCK_SIZE];
uint8_t omac_copy[NUM_SOURCE_ADDRESSES][AES_BLOCK_SIZE];
OMAC cmac[NUM_SOURCE_ADDRESSES];
OMAC cmac_copy[NUM_SOURCE_ADDRESSES];
AES128 cmac_cipher[NUM_SOURCE_ADDRESSES];
bool cmac_setup[NUM_SOURCE_ADDRESSES];
elapsedMillis cmac_timer[NUM_SOURCE_ADDRESSES];
uint8_t next_source_address_index = 0;
int current_sa = -1;

uint8_t encrypted_key[16];
uint8_t init_vector[16];
uint8_t aes_key[16];

#define EEPROM_NUM_ECU_SA_ADDR    161
#define EEPROM_ECU_LENGTH         24
#define EEPROM_ECU_ADDR           162

#define EEPROM_NUM_VEH_SA_ADDR    186
#define EEPROM_VEH_LENGTH         24
#define EEPROM_VEH_ADDR           187

#define EEPROM_SELF_SOURCE_ADDR   160


#define DM18_PGN                  54272
#define DM18_PUBLIC_KEY_TYPE      0x04
#define DM18_CMAC_TYPE            0x05
#define DM18_SESSION_KEY          0x02
#define DM18_CONFIRMATION_TYPE    0x06
#define DM18_RESET_TYPE           0x0F

bool key_confirmation_sent[256] = {};

#define COMPONENT_ID_PGN          65259
#define REQUEST_PGN               59904
#define TP_DT_PGN                 60160
#define TP_CM_PGN                 60416
#define CM_END_OF_MESSAGE_ACK     19
#define CM_CLEAR_TO_SEND          17
#define CM_REQUEST_TO_SEND        16
#define CM_BAM                    32
#define CM_ABORT                  255

#define DATA_SECURITY_PGN         54272
#define DATA_SECURITY_LONG_SEED   0x00
#define DATA_SECURITY_LONG_KEY    0x01
#define DATA_SECURITY_SESSION_KEY 0x02
#define DATA_SECURITY_CERTIFICATE 0x03
#define DATA_SECURITY_PUBLIC_KEY  0x04
#define DATA_SECURITY_CMAC        0x05
#define DATA_SECURITY_SERIAL_NUM  0x0C

#define NORMAL_PRIORITY 6
#define GLOBAL_ADDR     255
#define GATEWAY_SOURCE_ADDR  37

uint8_t self_source_addr;
uint8_t num_ecu_source_addresses;
uint8_t num_veh_source_addresses;
uint8_t ecu_source_addresses[NUM_SOURCE_ADDRESSES];
uint8_t veh_source_addresses[NUM_SOURCE_ADDRESSES];

void load_source_addresses(){
  EEPROM.get(EEPROM_SELF_SOURCE_ADDR,self_source_addr);
  EEPROM.get(EEPROM_NUM_ECU_SA_ADDR,num_ecu_source_addresses);
  EEPROM.get(EEPROM_NUM_VEH_SA_ADDR,num_veh_source_addresses);
  EEPROM.get(EEPROM_ECU_ADDR,ecu_source_addresses);
  EEPROM.get(EEPROM_VEH_ADDR,veh_source_addresses);

}

int get_cmac_index(uint8_t sa){
  for (int i = 0; i < num_veh_source_addresses; i++){
    if (veh_source_addresses[i] == (sa & 0xFF)){
      return i;
    }
  }
  return -1;
}

// // Use the index from the source address array to 
int get_sa_index(uint8_t sa){
  int sa_index = -1;
  for (uint16_t i = 0; i < NUM_SOURCE_ADDRESSES; i++){
    if (sa == source_addresses[i]){
      sa_index = i;
      break;
    }
  }
  
  if (sa_index < 0) {
    source_addresses[next_source_address_index] = sa;
    sa_index = next_source_address_index;
    next_source_address_index++;
    if (next_source_address_index >= NUM_SOURCE_ADDRESSES) {
      Serial.println("Source Address Space Exceeded");
      next_source_address_index = 0;
    }
  }
  //Serial.printf("Index for %02X is %d\n",sa,sa_index);
  return sa_index;
}

uint32_t pgn;
uint8_t sa;
uint8_t da;
uint8_t dlc;
uint8_t priority;


// A terribly inefficent way to setup transport layer buffers,
// But it's on a teensy, so who cares
#define J1939_MAX_LENGTH 1785
uint8_t tp_messages[NUM_SOURCE_ADDRESSES][NUM_DESTINATION_ADDRESSES][J1939_MAX_LENGTH];
uint8_t j1939_data[J1939_MAX_LENGTH];  
uint32_t j1939_pgn;
uint8_t j1939_sa;
uint8_t j1939_da;

uint8_t message_for_cmac[16];

//Keep track of transport messages
uint8_t tp_message_state[NUM_SOURCE_ADDRESSES][NUM_DESTINATION_ADDRESSES][10];
#define PACKET_COUNTER_INDEX 0
#define TOTAL_COUNT_INDEX    1
#define TOTAL_BYTE_INDEX     2 //Takes 2 bytes
#define PGN_INDEX            4 //Takes 4 bytes
#define BAM_CTS_RTS_INDEX    8
#define COUNT_TO_SEND_INDEX  9

#define BAM_TYPE             0
#define CTS_RTS_TYPE         1


void setup_aes_key(uint8_t cmac_index, uint8_t *init_vector, uint8_t *aes_key){
  //Serial.printf("Set Key for CMAC %0d\n",cmac_index);
  cmac_cipher[cmac_index].setKey(aes_key, sizeof(aes_key));
  cmac[cmac_index].setBlockCipher(&cmac_cipher[cmac_index]);
  
  Serial.println("Initializing CMAC");
  cmac[cmac_index].initFirst(omac[cmac_index]);
  memset(omac[cmac_index],0,sizeof(omac[cmac_index]));
  cmac[cmac_index].update(omac[cmac_index],init_vector,sizeof(init_vector));
  cmac_setup[cmac_index] = true;
  Serial.printf("Setting cmac_setup[%d] to true.\n",cmac_index);
  cmac_timer[cmac_index] = 0;
}


void print_bytes(byte bytes_to_print[], int array_size){
  for (uint16_t i = 0; i < array_size; i++)
  { 
    char hex_digit[3];
    sprintf(hex_digit,"%02X",bytes_to_print[i]);
    Serial.print(hex_digit);
  }
  Serial.println();
}

void send_frame(uint32_t pgn, uint8_t dest, uint8_t src, uint8_t *data, uint8_t dlc, uint8_t priority){
  CAN_message_t msg;
  msg.len = dlc & 0x0F;
  msg.id = priority << 26 ;
  //Serial.printf("PGN to send: %04X\n",pgn);
  msg.id += pgn << 8;
  if (pgn < 0xF000){
    msg.id += dest << 8;
  }
  msg.id += src;
  //Serial.printf("Sending ID %08X ",msg.id);
  //print_bytes(data,8);
  memcpy(&msg.buf[0], data, dlc);
  msg.flags.extended = 1;
  msg.seq = 1; // Puts the message in the queue to be sent.
  vehicle_can.write(msg);
  //vehicle_can.events();
}

void send_multi_frame(uint8_t dest, uint8_t src, uint8_t *data, uint8_t start_packet, uint8_t packets_to_send){  
  for (uint8_t i = start_packet; i < packets_to_send; i++){
    uint8_t data_to_send[8];
    data_to_send[0] = i+1;
    memcpy(&data_to_send[1],&data[i*7],7);
    send_frame(TP_DT_PGN, dest, src, data_to_send, sizeof(data_to_send), NORMAL_PRIORITY);
  }
}

void send_public_key_request(uint8_t da){
  uint8_t data_to_send[2];
  data_to_send[0] = 0; //Zero Length for a request
  data_to_send[1] = DM18_PUBLIC_KEY_TYPE;
  send_frame(DM18_PGN, da, self_source_addr, data_to_send, sizeof(data_to_send), NORMAL_PRIORITY);
}

void send_public_key(uint8_t da){
  
  uint8_t data_to_send[66];
  data_to_send[0] = 64; 
  data_to_send[1] = DM18_PUBLIC_KEY_TYPE;
  memcpy(&data_to_send[2], &own_public_key[0], sizeof(own_public_key));
  uint8_t packets_to_send = sizeof(data_to_send)/7 + bool(sizeof(data_to_send)%7);
  uint8_t setup_to_send[8];
  setup_to_send[0] = CM_BAM;
  setup_to_send[1] = sizeof(data_to_send) & 0xFF;
  setup_to_send[2] = (sizeof(data_to_send) & 0xFF00) >> 8;
  setup_to_send[3] = packets_to_send;
  setup_to_send[4] = 0xFF; //SAE specified
  setup_to_send[5] = (DM18_PGN & 0xFF);
  setup_to_send[6] = (DM18_PGN & 0xFF00) >> 8;
  setup_to_send[7] = (DM18_PGN & 0x030000) >> 16;
  //BAM
  send_frame(TP_CM_PGN, da, self_source_addr, setup_to_send, sizeof(setup_to_send), NORMAL_PRIORITY);
  uint8_t start_packet = 0;
  //Send to destination 
  send_multi_frame(da, self_source_addr, data_to_send, start_packet, packets_to_send);
}

void send_session_key_request(uint8_t da){
  uint8_t data_to_send[2];
  data_to_send[0] = 0; //Zero Length for a request
  data_to_send[1] = DM18_SESSION_KEY;
  send_frame(DM18_PGN, da, self_source_addr, data_to_send, sizeof(data_to_send), NORMAL_PRIORITY);
}

void send_session_key(uint8_t *encrypted_key, uint8_t *iv, uint8_t da){
  uint8_t data_to_send[28];
  data_to_send[0] = 26; 
  data_to_send[1] = DM18_SESSION_KEY;
  memcpy(&data_to_send[2], &encrypted_key[0], 16);
  memcpy(&data_to_send[18], &iv[0], 10);
  uint8_t packets_to_send = sizeof(data_to_send)/7 + bool(sizeof(data_to_send)%7);
  uint8_t setup_to_send[8];
  setup_to_send[0] = CM_BAM;
  setup_to_send[1] = sizeof(data_to_send) & 0xFF;
  setup_to_send[2] = (sizeof(data_to_send) & 0xFF00) >> 8;
  setup_to_send[3] = packets_to_send;
  setup_to_send[4] = 0xFF; //SAE specified
  setup_to_send[5] = (DM18_PGN & 0xFF);
  setup_to_send[6] = (DM18_PGN & 0xFF00) >> 8;
  setup_to_send[7] = (DM18_PGN & 0x030000) >> 16;
  //BAM
  send_frame(TP_CM_PGN, da, self_source_addr, setup_to_send, sizeof(setup_to_send), NORMAL_PRIORITY);
  uint8_t start_packet = 0;
  //Send to destination 
  send_multi_frame(da, self_source_addr, data_to_send, start_packet, packets_to_send);
}

void send_key_confirmation(uint8_t *encrypted_msg, uint8_t da){
  uint8_t data_to_send[18];
  data_to_send[0] = 16; 
  data_to_send[1] = DM18_CONFIRMATION_TYPE;
  memcpy(&data_to_send[2], &encrypted_msg[0], 16);
  uint8_t packets_to_send = sizeof(data_to_send)/7 + bool(sizeof(data_to_send)%7);
  uint8_t setup_to_send[8];
  setup_to_send[0] = CM_BAM;
  setup_to_send[1] = sizeof(data_to_send) & 0xFF;
  setup_to_send[2] = (sizeof(data_to_send) & 0xFF00) >> 8;
  setup_to_send[3] = packets_to_send;
  setup_to_send[4] = 0xFF; //SAE specified
  setup_to_send[5] = (DM18_PGN & 0xFF);
  setup_to_send[6] = (DM18_PGN & 0xFF00) >> 8;
  setup_to_send[7] = (DM18_PGN & 0x030000) >> 16;
  //BAM
  send_frame(TP_CM_PGN, da, self_source_addr, setup_to_send, sizeof(setup_to_send), NORMAL_PRIORITY);
  uint8_t start_packet = 0;
  //Send to destination 
  send_multi_frame(da, self_source_addr, data_to_send, start_packet, packets_to_send);
}

void send_clear_to_send(uint32_t j1939_pgn, uint8_t packets, uint8_t next, uint8_t da){
  uint8_t data_to_send[8];
  data_to_send[0] = CM_CLEAR_TO_SEND;
  data_to_send[1] = packets;
  data_to_send[2] = next;
  data_to_send[3] = 0xFF; //SAE specified
  data_to_send[4] = 0xFF; //SAE specified
  data_to_send[5] = (j1939_pgn & 0xFF);
  data_to_send[6] = (j1939_pgn & 0xFF00) >> 8;
  data_to_send[7] = (j1939_pgn & 0x030000) >> 16;
  send_frame(TP_CM_PGN, da, self_source_addr, data_to_send, sizeof(data_to_send), NORMAL_PRIORITY); 
}

void send_end_of_msg_ack(uint32_t j1939_pgn, uint8_t packets, uint16_t num_bytes, uint8_t da){
  uint8_t data_to_send[8];
  data_to_send[0] = CM_END_OF_MESSAGE_ACK;
  data_to_send[1] = num_bytes & 0xFF;
  data_to_send[2] = (num_bytes & 0xFF00) >> 8;
  data_to_send[3] = packets;
  data_to_send[4] = 0xFF; //SAE specified
  data_to_send[5] = (j1939_pgn & 0xFF);
  data_to_send[6] = (j1939_pgn & 0xFF00) >> 8;
  data_to_send[7] = (j1939_pgn & 0x030000) >> 16;
  send_frame(TP_CM_PGN, da, self_source_addr, data_to_send, sizeof(data_to_send), NORMAL_PRIORITY);
}

void send_component_id(uint8_t dest){
  char comp_id[4+sizeof(model_string)+1+sizeof(serial_string)];
  memset(comp_id,0xFF,sizeof(comp_id));
  strncpy(&comp_id[0],'CSU*',4);
  strncpy(&comp_id[4],model_string,sizeof(model_string));
  comp_id[4+sizeof(model_string)] = '*';
  strncpy(&comp_id[4+sizeof(model_string)+1],serial_string,sizeof(serial_string));
  comp_id[sizeof(comp_id) - 1] = '*';
  Serial.println(comp_id);
  int sa_index = get_sa_index(dest);
  uint8_t da_index;
  uint8_t packets_to_send = sizeof(comp_id)/7 + bool(sizeof(comp_id)%7);;
  uint8_t data_to_send[8];
  data_to_send[0] = CM_BAM;
  data_to_send[1] = sizeof(comp_id) & 0xFF;
  data_to_send[2] = (sizeof(comp_id) & 0xFF00) >> 8;
  data_to_send[3] = packets_to_send;
  data_to_send[4] = 0xFF; //SAE specified
  data_to_send[5] = (COMPONENT_ID_PGN & 0xFF);
  data_to_send[6] = (COMPONENT_ID_PGN & 0xFF00) >> 8;
  data_to_send[7] = (COMPONENT_ID_PGN & 0x030000) >> 16;
  //BAM
  send_frame(TP_CM_PGN, dest, self_source_addr, data_to_send, sizeof(data_to_send), NORMAL_PRIORITY);
  uint8_t start_packet = 0;
  uint8_t temp_buffer[sizeof(comp_id)];
  memcpy(&temp_buffer[0],comp_id,sizeof(comp_id));
  send_multi_frame(dest, self_source_addr, comp_id, start_packet, packets_to_send);
}

int parseJ1939(CAN_message_t msg){
  dlc = msg.len;
  sa = (msg.id & 0xFF);
  uint32_t pf = (msg.id & 0x3FF0000) >> 16;
  if (pf < 240){
    da = (msg.id & 0x00FF00) >> 8;
    pgn = pf << 8;
  }
  else{
    da = 0xff;  
    pgn = (msg.id & 0x3FFFF00) >> 8;
  }
  
  int sa_index = get_sa_index(sa);
  if (sa_index < 0) {
    Serial.printf("No Index for SA %02X\n",sa);
    return -1;
  }
    
  uint8_t da_index;
  if (da == 0xFF) da_index = 0;
  else if (da == self_source_addr) da_index = 1;
  else if (da == (self_source_addr & 0x7F)) da_index = 2;
  else {
    Serial.printf("Message for %02X not in da_index\n",da);
    return -1; // The message is not for us. 
  }

  memcpy(&j1939_data[0],&msg.buf[0],dlc);
  j1939_sa = sa;
  j1939_da = da;
  j1939_pgn = pgn;

  if (pgn == REQUEST_PGN){
    //Serial.print("Found Request PGN: ");
    //print_bytes(msg.buf,msg.len);
    j1939_pgn = (msg.buf[2] << 16) + (msg.buf[1] << 8) + msg.buf[0];
    // Send a response if the PGN is supported
    //Serial.printf("%X == %X\n",j1939_pgn,COMPONENT_ID_PGN);
    if (j1939_pgn == COMPONENT_ID_PGN){
      send_component_id(GLOBAL_ADDR);
    }
  }
  else if (pgn == TP_DT_PGN){
    memcpy(&tp_messages[sa_index][da_index][7*(msg.buf[0]-1)],&msg.buf[1],7);
    tp_message_state[sa_index][da_index][PACKET_COUNTER_INDEX]++;
//    Serial.print("Transport Layer Packets Received: ");
//    print_bytes(msg.buf,8);
//    Serial.print(tp_message_state[sa_index][da_index][PACKET_COUNTER_INDEX]);
//    Serial.print(" == ");
//    Serial.println(tp_message_state[sa_index][da_index][TOTAL_COUNT_INDEX]);
  
    if (tp_message_state[sa_index][da_index][PACKET_COUNTER_INDEX] == 
        tp_message_state[sa_index][da_index][TOTAL_COUNT_INDEX]){
      // data transfer complete
      uint16_t num_bytes;
      memcpy(&num_bytes, &tp_message_state[sa_index][da_index][TOTAL_BYTE_INDEX], 2);
      uint8_t packets = tp_message_state[sa_index][da_index][1];
      memcpy(&j1939_pgn, &tp_message_state[sa_index][da_index][PGN_INDEX], 4);
      if (tp_message_state[sa_index][da_index][BAM_CTS_RTS_INDEX] == CTS_RTS_TYPE &&  da == self_source_addr){
        // Only respond to things sent to us.
        send_end_of_msg_ack(j1939_pgn,num_bytes,packets,sa);
      }
      memcpy(&j1939_data[0], &tp_messages[sa_index][da_index][0],num_bytes);
      j1939_sa = sa;
      j1939_da = da;
      tp_message_state[sa_index][da_index][PACKET_COUNTER_INDEX] = 0;
      tp_message_state[sa_index][da_index][TOTAL_COUNT_INDEX] = 255;
//      Serial.print("TPDATA: ");
//      print_bytes(j1939_data,num_bytes);
      return num_bytes;  
    }  
  }
  else if (pgn == TP_CM_PGN){
    //Serial.print("Transport Layer Control Received: ");
    //print_bytes(msg.buf,8);
    uint8_t control_byte = msg.buf[0];
    if (control_byte == CM_BAM){//Broadcast Announce Message
      tp_message_state[sa_index][da_index][BAM_CTS_RTS_INDEX] = BAM_TYPE;
      tp_message_state[sa_index][da_index][PACKET_COUNTER_INDEX] = 0;
      tp_message_state[sa_index][da_index][TOTAL_COUNT_INDEX] = msg.buf[3];
      uint16_t msg_size = msg.buf[1] + (uint16_t(msg.buf[2]) << 8);
      memcpy(&tp_message_state[sa_index][da_index][TOTAL_BYTE_INDEX], &msg_size, 2);
      j1939_pgn = (msg.buf[7] << 16) + (msg.buf[6] << 8) + msg.buf[5];
      memcpy(&tp_message_state[sa_index][da_index][PGN_INDEX], &j1939_pgn, 4);
      memset(&tp_messages[sa_index][da_index][0],0xFF,J1939_MAX_LENGTH);
    }
    else if (control_byte == CM_REQUEST_TO_SEND){ //Request to Send
      tp_message_state[sa_index][da_index][BAM_CTS_RTS_INDEX] = CTS_RTS_TYPE;
      uint8_t next = 0;
      tp_message_state[sa_index][da_index][PACKET_COUNTER_INDEX] = next;
      uint8_t total_packets = msg.buf[3];
      tp_message_state[sa_index][da_index][TOTAL_COUNT_INDEX] = total_packets;
      uint8_t response_packets = msg.buf[4];
      tp_message_state[sa_index][da_index][COUNT_TO_SEND_INDEX] = response_packets;
      
      uint16_t msg_size = msg.buf[1] + (uint16_t(msg.buf[2]) << 8);
      memcpy(&tp_message_state[sa_index][da_index][TOTAL_BYTE_INDEX], &msg_size, 2);
      j1939_pgn = (msg.buf[7] << 16) + (msg.buf[6] << 8) + msg.buf[5];
      memcpy(&tp_message_state[sa_index][da_index][PGN_INDEX], &j1939_pgn, 4);
      memset(&tp_messages[sa_index][da_index][0],0xFF,J1939_MAX_LENGTH);
      if (da == self_source_addr){
        // Only respond to things sent to us.
        send_clear_to_send(j1939_pgn,min(total_packets,response_packets),next,sa);
      }
    }
    else if (control_byte == CM_CLEAR_TO_SEND){ //Clear to Send
      uint16_t packets_to_send = msg.buf[1] + (uint16_t(msg.buf[2]) << 8);
      uint8_t start_packet = msg.buf[2];
      j1939_pgn = (msg.buf[7] << 16) + (msg.buf[6] << 8) + msg.buf[5];
      send_multi_frame(da, sa, tp_messages[sa_index][da_index], start_packet, packets_to_send);
    }
    else if (control_byte == CM_END_OF_MESSAGE_ACK){ //End of message ACK

    }
    else if (control_byte == CM_ABORT){ //Abort
      //return 0;
    }  
  }
  
  return dlc; 
}

void setup_CMAC(uint8_t sa_index){
  cmac_setup[sa_index] = false;
  CAN_message_t msg;
  sa = (veh_source_addresses[sa_index] & 0x7F) + 0x80;
  msg.len = 2;
  msg.id = 6 << 26 ;
  msg.id += DM18_PGN << 8;
  msg.id += sa << 8; // Destination address
  msg.id += self_source_addr;
  msg.buf[0] = 0;
  msg.buf[1] = 0x0F; // Abort
  Serial.printf("Sending ID %08X ",msg.id);
  print_bytes(msg.buf,msg.len);
  msg.flags.extended = 1;
  vehicle_can.write(msg);
};

void update_cmac(uint8_t cmac_index, CAN_message_t msg) {
  memset(message_for_cmac, 0, sizeof(message_for_cmac));
  memcpy(&message_for_cmac[0], &msg.id, 4);
  message_for_cmac[4] = msg.len;
  memcpy(&message_for_cmac[5], &msg.buf[0], msg.len);
  cmac[cmac_index].update(omac[cmac_index], message_for_cmac, sizeof(message_for_cmac));
}

void send_cmac(uint8_t sa, uint8_t da, uint8_t cmac_index){
//Make a copy to produce an intermediate result
  memcpy(&cmac_copy[cmac_index], &cmac[cmac_index], sizeof(cmac[cmac_index]));
  memcpy(&omac_copy[cmac_index], &omac[cmac_index], sizeof(omac[cmac_index]));
  //Serial.println("CMAC Copy Finalize:");
  cmac_copy[cmac_index].finalize(omac_copy[cmac_index]);
  //print_bytes(omac_copy[i], sizeof(omac_copy[i]));
  CAN_message_t vehicle_msg;
  vehicle_msg.id = 0x00D40000; //DM18 Message with highest priority
  vehicle_msg.id += da << 8;
  vehicle_msg.id += sa;
  vehicle_msg.len = 8;
  vehicle_msg.flags.extended = 1;
  vehicle_msg.buf[0] = 6; //Length
  vehicle_msg.buf[1] = DM18_CMAC_TYPE;
  memcpy(&vehicle_msg.buf[2], omac_copy[cmac_index], 6);
  vehicle_can.write(vehicle_msg);
  //Serial.print("Sent 6 bytes of OMAC: ");
  //print_bytes(vehicle_msg.buf, 6);
}

void compare_cmacs(uint8_t cmac_index, uint8_t *cmac_value) {
  send_cmac(self_source_addr, veh_source_addresses[cmac_index], cmac_index);

  if (!memcmp(omac_copy[cmac_index], cmac_value, 6)) {
    Serial.println("CMACs Matched.");
  }
  else {
    Serial.println("CMACs did not match.");
  }
}
