#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <thread>
#include <future>
#include <vector>
#include <fstream>
#include <chrono>
#include <ctime>
#include "SchedulerHandler.h"
#include <unistd.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "TilesDims.h"
#include <filesystem>
#include <experimental/filesystem>
#include <stdio.h> 
#include <sys/ipc.h> 
#include <sys/msg.h> 
#include <string>
#include <tuple>
#include <sstream>
#include <iomanip> 

#include <stdint.h>
#include <stdio.h>
#include <picoquic.h>
#include <picoquic_utils.h>
#include <picosocks.h>
#include <autoqlog.h>
#include <picoquic_packet_loop.h>
#include "picoquic_sample.h"
  

using namespace std;
using namespace cv;

int iterations=20;

typedef struct st_sample_client_stream_ctx_t {
    struct st_sample_client_stream_ctx_t* next_stream;
    uint64_t stream_id;
    FILE* F;
    size_t bytes_received;
    size_t total_bytes_received;
    uint64_t remote_error;
    size_t message_sent_length;
    unsigned int is_file_open : 1;
    unsigned int is_stream_reset : 1;
    unsigned int is_stream_finished : 1;
      unsigned int is_message_sent : 1;
} sample_client_stream_ctx_t;

typedef struct st_sample_client_ctx_t {
    picoquic_cnx_t* cnx;
    char const* default_dir;
    sample_client_stream_ctx_t* first_stream;
    sample_client_stream_ctx_t* last_stream;
    int nb_files_received;
    int nb_files_failed;
    int is_disconnected;
} sample_client_ctx_t;


static int sample_client_create_stream(picoquic_cnx_t* cnx,
    sample_client_ctx_t* client_ctx)
{
            int ret = 0;
        sample_client_stream_ctx_t* stream_ctx = (sample_client_stream_ctx_t*) malloc(sizeof(sample_client_stream_ctx_t));

        if (stream_ctx == NULL) {
            fprintf(stdout, "Memory Error, cannot create stream\n");
            ret = -1;
        }
        else {
            memset(stream_ctx, 0, sizeof(sample_client_stream_ctx_t));

            if (client_ctx->first_stream == NULL) {
                client_ctx->first_stream = stream_ctx;
                client_ctx->last_stream = stream_ctx;
            }
            else {
                client_ctx->last_stream->next_stream = stream_ctx;
                client_ctx->last_stream = stream_ctx;
            }

            stream_ctx->stream_id = picoquic_get_next_local_stream_id(client_ctx->cnx, 0);

            /* Mark the stream as active. The callback will be asked to provide data when 
            * the connection is ready. */
            ret = picoquic_mark_active_stream(cnx, stream_ctx->stream_id, 1, stream_ctx);
            if (ret != 0) {
                fprintf(stdout, "Error %d, cannot initialize stream\n", ret);
            }
            else {
                printf("Opened stream with ID %ld\n", stream_ctx->stream_id);
            }
        }
        return ret;
}



static void sample_client_report(sample_client_ctx_t* client_ctx)
{
    sample_client_stream_ctx_t* stream_ctx = client_ctx->first_stream;

    while (stream_ctx != NULL) {
        char const* status;
        if (stream_ctx->is_stream_finished) {
            status = "complete";
        }
        else if (stream_ctx->is_stream_reset) {
            status = "reset";
        }
        else {
            status = "unknown status";
        }
        printf(" %s, received %zu bytes", status, stream_ctx->bytes_received);
        if (stream_ctx->is_stream_reset && stream_ctx->remote_error != PICOQUIC_SAMPLE_NO_ERROR){
            char const* error_text = "unknown error";
            switch (stream_ctx->remote_error) {
            case PICOQUIC_SAMPLE_INTERNAL_ERROR:
                error_text = "internal error";
                break;
            case PICOQUIC_SAMPLE_NAME_TOO_LONG_ERROR:
                error_text = "internal error";
                break;
            case PICOQUIC_SAMPLE_NO_SUCH_FILE_ERROR:
                error_text = "no such file";
                break;
            case PICOQUIC_SAMPLE_FILE_READ_ERROR:
                error_text = "file read error";
                break;
            case PICOQUIC_SAMPLE_FILE_CANCEL_ERROR:
                error_text = "cancelled";
                break;
            default:
                break;
            }
            printf(", error 0x%" PRIx64 " -- %s", stream_ctx->remote_error, error_text);
        }
        printf("\n");
        stream_ctx = stream_ctx->next_stream;
    }
}

static void sample_client_free_context(sample_client_ctx_t* client_ctx)
{
    sample_client_stream_ctx_t* stream_ctx;

    while ((stream_ctx = client_ctx->first_stream) != NULL) {
        client_ctx->first_stream = stream_ctx->next_stream;
        if (stream_ctx->F != NULL) {
            (void)picoquic_file_close(stream_ctx->F);
        }
        free(stream_ctx);
    }
    client_ctx->last_stream = NULL;
}


int sample_client_callback(picoquic_cnx_t* cnx,
    uint64_t stream_id, uint8_t* bytes, size_t length,
    picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx)
{
    int ret = 0;
    sample_client_ctx_t* client_ctx = (sample_client_ctx_t*)callback_ctx;
    sample_client_stream_ctx_t* stream_ctx = (sample_client_stream_ctx_t*)v_stream_ctx;

    if (client_ctx == NULL) {
        /* This should never happen, because the callback context for the client is initialized 
         * when creating the client connection. */
        return -1;
    }

    if (ret == 0) {
        switch (fin_or_event) {
        case picoquic_callback_stream_data:
            printf("Data arrived on stream %ld\n", stream_ctx->stream_id);
            if (stream_ctx == NULL) {
                /* This is unexpected, as all contexts were declared when initializing the connection. */
                return -1;
            } else if (stream_ctx->is_stream_reset || stream_ctx->is_stream_finished) {
                /* Unexpected: receive after fin */
                return -1;
            } else {
                 if (!stream_ctx->is_file_open) {
                    // Handle filename first
                    char filename[1024];
                    size_t filename_length = length > sizeof(filename) - 1 ? sizeof(filename) - 1 : length;

                    // Print raw data received
                    printf("Received %zu bytes: ", length);
                    for (size_t i = 0; i < length; ++i) {
                        printf("%02x ", bytes[i]);
                    }
                    printf("\n");

                    // Extract filename
                    memcpy(filename, bytes, filename_length);
                    filename[filename_length] = '\0';  // Null-terminate the filename

                    // Debug print for filename
                    printf("Filename received (length %zu): '%s'\n", filename_length, filename);

                    char file_path[1024];
                    size_t dir_len = strlen(client_ctx->default_dir);
                   
                    /* Copy the default directory path into file_path */
                    if (dir_len > 0 && dir_len < sizeof(file_path)) {
                        memcpy(file_path, client_ctx->default_dir, dir_len);
                        if (file_path[dir_len - 1] != PICOQUIC_FILE_SEPARATOR[0]) {
                            file_path[dir_len] = PICOQUIC_FILE_SEPARATOR[0];
                            dir_len++;
                        }
                    }

                    // Open the file for writing
                    snprintf(file_path + dir_len, sizeof(file_path) - dir_len, filename);
                    stream_ctx->F = picoquic_file_open(file_path, "wb");
                    if (stream_ctx->F == NULL) {
                        fprintf(stderr, "Could not open the file: %s\n", file_path);
                        return -1;
                    }
                    stream_ctx->is_file_open = 1;

                    printf("Opened file: %s\n", filename);

                    // Continue processing any remaining data
                    // if (length > filename_length) {
                    //     bytes += filename_length;
                    //     length -= filename_length;
                    // } else {
                    //     length = 0;
                    // }
                }
                if(length > 9 ){
                /* Write the received bytes to the file */
                printf("Writing %zu bytes to the disk\n", length);
                if (fwrite(bytes, length, 1, stream_ctx->F) != 1) {
                    /* Could not write file to disk */
                    fprintf(stderr, "Could not write data to disk.\n");
                    return -1;
                } else {
                    stream_ctx->bytes_received += length;
                    printf("Current File's bytes received: %ld\n", stream_ctx->bytes_received);
                }
                }
                else if (length == 9 && strncmp((const char*)bytes, "***EOF***", 9) == 0) {
                            printf("End of file received: ***EOF***\n");
                             picoquic_file_close(stream_ctx->F);
                             stream_ctx->F = NULL;
                             stream_ctx->is_file_open = 0;
                             client_ctx->nb_files_received++;
                             stream_ctx->total_bytes_received += stream_ctx->bytes_received;
                             stream_ctx->bytes_received = 0;
                            printf("Total files received: %d\n", client_ctx->nb_files_received);
                             printf("Total bytes received: %ld\n", stream_ctx->total_bytes_received);
                    }
                
            }
            

            break;

        case picoquic_callback_stream_fin:
            printf("Stream finished on stream %ld\n", stream_ctx->stream_id);

            if (stream_ctx != NULL && stream_ctx->F != NULL) {
                /* Close the file when the stream is finished */
                picoquic_file_close(stream_ctx->F);
                stream_ctx->F = NULL; /* Reset the file pointer */
                //stream_ctx->is_stream_finished = 1;
                client_ctx->nb_files_received++;
                printf("Stream finished. Total files received: %d\n", client_ctx->nb_files_received);

                
            }
            break;
        case picoquic_callback_stop_sending: /* Should not happen, treated as reset */
            /* Mark stream as abandoned, close the file, etc. */
            printf("ResetStream\n");
            picoquic_reset_stream(cnx, stream_id, 0);
            /* Fall through */
        case picoquic_callback_stream_reset: /* Server reset stream #x */
            printf("ResetStream1\n");
            if (stream_ctx == NULL) {
                /* This is unexpected, as all contexts were declared when initializing the
                 * connection. */
                return -1;
            }
            else if (stream_ctx->is_stream_reset || stream_ctx->is_stream_finished) {
                /* Unexpected: receive after fin */
                return -1;
            }
            else {
                stream_ctx->remote_error = picoquic_get_remote_stream_error(cnx, stream_id);
                stream_ctx->is_stream_reset = 1;
                client_ctx->nb_files_failed++;

                // if ((client_ctx->nb_files_received + client_ctx->nb_files_failed) >= client_ctx->nb_files) {
                //     /* everything is done, close the connection */
                //     fprintf(stdout, "All done, closing the connection.\n");
                    ret = picoquic_close(cnx, 0);
                //}
            }
            break;
        case picoquic_callback_stateless_reset:
            printf("ResetStream2\n");
        case picoquic_callback_close: /* Received connection close */
            printf("I got connection close\n");
            break;
        case picoquic_callback_application_close: /* Received application close */
            fprintf(stdout, "Connection closed.\n");
            /* Mark the connection as completed */
            client_ctx->is_disconnected = 1;
            /* Remove the application callback */
            picoquic_set_callback(cnx, NULL, NULL);
            break;
        case picoquic_callback_version_negotiation:
            /* The client did not get the right version.
             * TODO: some form of negotiation?
             */
            fprintf(stdout, "Received a version negotiation request:");
            for (size_t byte_index = 0; byte_index + 4 <= length; byte_index += 4) {
                uint32_t vn = 0;
                for (int i = 0; i < 4; i++) {
                    vn <<= 8;
                    vn += bytes[byte_index + i];
                }
                fprintf(stdout, "%s%08x", (byte_index == 0) ? " " : ", ", vn);
            }
            fprintf(stdout, "\n");
            break;
        case picoquic_callback_stream_gap:
            /* This callback is never used. */
            break;
        
         case picoquic_callback_prepare_to_send:
        /* Active sending API */
        if (stream_ctx == NULL) {
            /* Decidedly unexpected */
            return -1;
        } else if (!stream_ctx->is_message_sent) {
            stream_ctx->total_bytes_received = 0;
            uint8_t* buffer;
            const char* message = "Hi Server";
            size_t message_len = strlen(message);
            size_t available = message_len - stream_ctx->message_sent_length;
            int is_fin = 1;

            /* The length parameter marks the space available in the packet */
            if (available > length) {
                available = length;
                is_fin = 0;
            }

            /* Provide the buffer to send the "Hi Server" message */
            buffer = picoquic_provide_stream_data_buffer(bytes, available, is_fin, !is_fin);
            if (buffer != NULL) {
                /* Copy the "Hi Server" message into the buffer */
                memcpy(buffer, message + stream_ctx->message_sent_length, available);
                stream_ctx->message_sent_length += available;
                stream_ctx->is_message_sent = is_fin;  // Mark as done if the whole message was sent
            } else {
                ret = -1;  // Error handling if buffer could not be retrieved
            }
        }
        break;


        case picoquic_callback_almost_ready:
            fprintf(stdout, "Connection to the server completed, almost ready.\n");
            break;
        case picoquic_callback_ready:
            /* TODO: Check that the transport parameters are what the sample expects */
            fprintf(stdout, "Connection to the server confirmed.\n");
            break;
        default:
            /* unexpected -- just ignore. */
            break;
        }
    }

    return ret;
}

/* Sample client,  loop call back management.
 * The function "picoquic_packet_loop" will call back the application when it is ready to
 * receive or send packets, after receiving a packet, and after sending a packet.
 * We implement here a minimal callback that instruct  "picoquic_packet_loop" to exit
 * when the connection is complete.
 */

static int sample_client_loop_cb(picoquic_quic_t* quic, picoquic_packet_loop_cb_enum cb_mode, 
    void* callback_ctx, void * callback_arg)
{
    int ret = 0;
    sample_client_ctx_t* cb_ctx = (sample_client_ctx_t*)callback_ctx;

    if (cb_ctx == NULL) {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else {
        switch (cb_mode) {
        case picoquic_packet_loop_ready:
            fprintf(stdout, "Waiting for packetsss.\n");
            break;
        case picoquic_packet_loop_after_receive:
            printf("after receive\n");
            break;
        case picoquic_packet_loop_after_send:
            printf("aFTER SEND\n");
            if (cb_ctx->is_disconnected) {
                ret = PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
            }
            break;
        case picoquic_packet_loop_port_update:
            break;
        default:
            ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
            break;
        }
    }
    return ret;
}

/* Prepare the context used by the simple client:
 * - Create the QUIC context.
 * - Open the sockets
 * - Find the server's address
 * - Initialize the client context and create a client connection.
 */
static int sample_client_init(char const* server_name, int server_port, char const* default_dir,
    struct sockaddr_storage * server_address, picoquic_quic_t** quic, picoquic_cnx_t** cnx, sample_client_ctx_t *client_ctx)
{
    int ret = 0;
    char const* sni = PICOQUIC_SAMPLE_SNI;
    uint64_t current_time = picoquic_current_time();

    *quic = NULL;
    *cnx = NULL;

    /* Get the server's address */
    if (ret == 0) {
        int is_name = 0;

        ret = picoquic_get_server_address(server_name, server_port, server_address, &is_name);
        if (ret != 0) {
            fprintf(stderr, "Cannot get the IP address for <%s> port <%d>", server_name, server_port);
        }
        else if (is_name) {
            sni = server_name;
        }
    }

    /* Create a QUIC context. It could be used for many connections, but in this sample we
     * will use it for just one connection.
     * The sample code exercises just a small subset of the QUIC context configuration options:
     * - use files to store tickets and tokens in order to manage retry and 0-RTT
     * - set the congestion control algorithm to BBR
     * - enable logging of encryption keys for wireshark debugging.
     * - instantiate a binary log option, and log all packets.
     */
    if (ret == 0) {
        *quic = picoquic_create(1, NULL, NULL, NULL, PICOQUIC_SAMPLE_ALPN, NULL, NULL,
            NULL, NULL, NULL, current_time, NULL,
            NULL, NULL, 0);

        if (*quic == NULL) {
            fprintf(stderr, "Could not create quic context\n");
            ret = -1;
        }
            picoquic_set_default_congestion_algorithm(*quic, picoquic_bbr_algorithm);
        
    }
    /* Initialize the callback context and create the connection context.
     * We use minimal options on the client side, keeping the transport
     * parameter values set by default for picoquic. This could be fixed later.
     */

    if (ret == 0) {
        client_ctx->default_dir = default_dir;

        printf("Starting connection to %s, port %d\n", server_name, server_port);

        /* Create a client connection */
        *cnx = picoquic_create_cnx(*quic, picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)server_address, current_time, 0, sni, PICOQUIC_SAMPLE_ALPN, 1);

        if (*cnx == NULL) {
            fprintf(stderr, "Could not create connection context\n");
            ret = -1;
        }
        else {
            /* Document connection in client's context */
            client_ctx->cnx = *cnx;
            /* Set the client callback context */
            picoquic_set_callback(*cnx, sample_client_callback, client_ctx);
            /* Client connection parameters could be set here, before starting the connection. */
            ret = picoquic_start_client_cnx(*cnx);
            if (ret < 0) {
                fprintf(stderr, "Could not activate connection\n");
            }
            else {
                /* Printing out the initial CID, which is used to identify log files */
                picoquic_connection_id_t icid = picoquic_get_initial_cnxid(*cnx);
                printf("Initial connection ID: ");
                for (uint8_t i = 0; i < icid.id_len; i++) {
                    printf("%02x", icid.id[i]);
                }
                printf("\n");
            }
        }
    }

    return ret;
}




class EvictingQueue {
private:
    int _size;
    int drops;
    std::queue<std::string> _queue;
    int pollWaitTime;
    std::mutex mtx;
    std::condition_variable cv;

public:
    
    // Default constructor
    EvictingQueue() : _size(0), pollWaitTime(0) {
        cout<<"Evicting queue cosntructor"<<endl;
    }
    
    EvictingQueue(int size, int pollWaitTimeInMillis) : _size(size), drops(0), pollWaitTime(pollWaitTimeInMillis) {
         cout<<"Evicting queue cosntructor"<<endl;
    }

      // Copy constructor
    EvictingQueue(const EvictingQueue& other) {
        _size = other._size;
        drops = other.drops;
        pollWaitTime = other.pollWaitTime;
        // Copy the elements of the queue
        _queue = other._queue;
    }

    // Copy assignment operator
    EvictingQueue& operator=(const EvictingQueue& other) {
        if (this != &other) {
            _size = other._size;
            drops = other.drops;
            pollWaitTime = other.pollWaitTime;
            // Clear current queue
            while (!_queue.empty()) {
                _queue.pop();
            }
            // Copy elements from other queue
            std::queue<std::string> tmp = other._queue;
            while (!tmp.empty()) {
                _queue.push(tmp.front());
                tmp.pop();
            }
        }
        return *this;
    }

    void add(const std::string& element) {
        std::unique_lock<std::mutex> lock(mtx);
        if (_queue.size() < _size) {
            _queue.push(element);
        } else {
            _queue.pop();
            _queue.push(element);
            drops++;
        }
        cv.notify_all();
        cout<<"Added "<<element<<endl;
    }

    int getDrops() {
        return drops;
    }

    std::string take() {
        std::unique_lock<std::mutex> lock(mtx);
        // Check if the queue is empty
        while (_queue.empty()) {
        // Wait for a specified duration for the condition variable to be notified
        if (cv.wait_for(lock, std::chrono::milliseconds(pollWaitTime)) == std::cv_status::timeout) {
            // If wait times out, break the loop
            break;
        }
    }
        if (!_queue.empty()) {
            std::string element = _queue.front();
            _queue.pop();
            return element;
        }
        return NULL; // Or throw an exception for timeout
    }

    int size() {
        std::unique_lock<std::mutex> lock(mtx);
        return _queue.size();
    }
    void viewQueue() {
        std::unique_lock<std::mutex> lock(mtx);
        std::cout << "Contents of the queue:" << std::endl;
        
        // Create a copy of the queue
        std::queue<std::string> copyQueue = _queue;

        // Iterate over the elements of the copied queue and print them
        while (!copyQueue.empty()) {
            std::cout << copyQueue.front() << std::endl;
            copyQueue.pop();
        }
    }
};

class Stats {
    public:
    int delayInFGAndBGSegments = 0;
};

class Receiver { 

private:
    sockaddr_in socketAddress;
    int receiverChannel;
    std::string primaryFileSavingDirectory;
    std::string helperFileSavingDirectory;
    int receiverId;
    Stats stats;
    string serverIp;
    std::vector<char> nrlBytes;
    std::vector<char> endHeaderBytes;
    std::vector<char> midHeaderBytes;
    int serverPort;

    int receiveBufferSize = 2500;
    EvictingQueue& primaryReceiverQueue;
    EvictingQueue& helperReceiverQueue;
    ControlChannelHandler& controlChannelHandler;  // Changed to reference
    std::string networkLogFileName;

public:

    Receiver(const std::string& serverIp, int serverPort, const std::string& p_fileDirPath, const std::string& h_fileDirPath, int p_receiverId, EvictingQueue& p_queue, EvictingQueue& h_queue, ControlChannelHandler& CCHandler)
    : serverIp(serverIp), serverPort(serverPort), helperFileSavingDirectory(h_fileDirPath), primaryFileSavingDirectory(p_fileDirPath), receiverId(p_receiverId), primaryReceiverQueue(p_queue), helperReceiverQueue(h_queue), controlChannelHandler(CCHandler) {
        std::cout << "This is receiver class constructor" << std::endl;
    }

    
 // Function to extract header information from the file
    std::tuple<long long, std::string, std::string, std::streampos> readHeader(const std::string& fileName) {
    std::ifstream inFile(fileName, std::ios::binary);
    if (!inFile) {
        std::cerr << "Error: Couldn't open file for reading." << std::endl;
        return std::make_tuple(0L, "", "", 0);
    }

    inFile.seekg(0, std::ios::end); // Move file pointer to end
    std::streampos fileSize = inFile.tellg(); // Get file size

    std::string content;
    char ch;
    int underscoreCount = 0;
    bool foundFirstUnderscore = false;
    bool foundSecondUnderscore = false;
    bool foundThirdUnderscore = false;

    std::string timestampStr;
    std::string segmentIndexStr;
    std::string tilesStr;

    for (std::streampos i = fileSize - 1; i >= 0; i=i-1) {
        inFile.seekg(i);
        inFile.get(ch);
        if (ch == '_') {
            underscoreCount++;
            if (underscoreCount == 1 && !foundFirstUnderscore) {
                foundFirstUnderscore = true;
                continue; // Skip this underscore
            } else if (underscoreCount == 2 && !foundSecondUnderscore) {
                foundSecondUnderscore = true;
                continue; // Skip this underscore
            } else if (underscoreCount == 3 && !foundThirdUnderscore) {
                foundThirdUnderscore = true;
                continue; // Skip this underscore
            }
        }
        if (foundFirstUnderscore && !foundSecondUnderscore) {
            segmentIndexStr = ch + segmentIndexStr; // Collect segment index
        } else if (foundSecondUnderscore && !foundThirdUnderscore) {
            tilesStr = ch + tilesStr; // Collect tiles
        } else if (foundThirdUnderscore && timestampStr.size() < 13) {
            timestampStr = ch + timestampStr; // Collect timestamp
        }
    }

    inFile.close(); // Close the file

    // Convert collected strings to appropriate types
    long long timestamp = 0L;

    try {
        timestamp = std::stoll(timestampStr);
    } catch (const std::invalid_argument& e) {
        std::cerr << "Error: Invalid argument while parsing timestamp." << std::endl;
        return std::make_tuple(0L, "", "", 0);
    } catch (const std::out_of_range& e) {
        std::cerr << "Error: Out of range value while parsing timestamp." << std::endl;
        return std::make_tuple(0L, "", "", 0);
    }
    
    return std::make_tuple(timestamp, segmentIndexStr, tilesStr, fileSize);
}


    long long getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
        auto epoch = now_ms.time_since_epoch();
        long long timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
        return timestamp_ms;
        }

    // Function to create a header from Timestamp, SegmentIndex, and Filesize
    std::string createHeader(long long timestamp, std::string segmentIndex, std::streampos fileSize) {
        return  std::to_string(fileSize) + "_" + std::to_string(timestamp) + "_" + segmentIndex;
    }



    void receiveFile() {
        
        std::vector<char> rxBuffer(receiveBufferSize);
        std::vector<char> fileBytes;
        long videoSegmentSize = 0; 

        std::string filePath; 
        cout<<this->serverIp+"===="<<this->serverPort<<endl;
        int count=0;
        //for(int i=0; i<iterations; i++) {
            cout<<this->receiverId<<endl;
            char s3[4096];
        //     if(this->receiverId==0){
        //     sprintf(s3,"./quicinteropserver -listen:192.168.22.56 -name:%s -port:1221 -root:./ -upload:%s  -file:server.cert AND -key:server.key" , "localhost",primaryFileSavingDirectory.c_str());
        //    }else{
        //     sprintf(s3, "./quicinteropserver -listen:192.168.22.56 -name:%s -port:1222 -root:./ -upload:%s -file:server.cert AND -key:server.key","localhost",helperFileSavingDirectory.c_str());
        //    }
        string filename = "out" + to_string(i) + ".mp4";
        cout<<filename<<" :::: "<<endl;
        if(this->receiverId==0){
            sprintf(s3,"./sample client 127.0.0.1 1221 %s %s " ,primaryFileSavingDirectory.c_str(), filename.c_str());
        }else{
            sprintf(s3,"./sample client 127.0.0.1 1222 %s %s " ,helperFileSavingDirectory.c_str(), filename.c_str());
        }
        system(s3);
        long long timestamp;
            std::string segmentIndexStr;
            std::string tilesStr;
            std::streampos fileSize = 0;
             std::string header;
            // Determine which receiver is processing the message
            if (this->receiverId == 0) {
                filePath = primaryFileSavingDirectory + filename;
                std::tie(timestamp, segmentIndexStr, tilesStr,fileSize) = readHeader(filePath);
                primaryReceiverQueue.add(filePath);
                header = createHeader(timestamp, segmentIndexStr, fileSize);
                primaryReceiverQueue.viewQueue();
            } else {
                filePath = helperFileSavingDirectory + filename;
                std::tie(timestamp, segmentIndexStr, tilesStr,fileSize) = readHeader(filePath);
                header = createHeader(timestamp, segmentIndexStr, fileSize);
                helperReceiverQueue.add(filePath);
                helperReceiverQueue.viewQueue();
            }

              // Display the results
            std::cout << "Timestamp: " << timestamp << " Of file " <<filePath<<std::endl;
            std::cout << "Segment Index: " << segmentIndexStr << std::endl;
            std::cout << "Foreground Tiles: " << tilesStr << std::endl;
            std::cout << "File size: " << fileSize << " bytes" << std::endl;

            std::cout << "Header: " << header << std::endl;

            controlChannelHandler.sendStats(header);
       // }

           cout<<"++++++++++++++++++++OUT"<<endl;
       
    }
    

    void run() {
         int ret = 0;
    struct sockaddr_storage server_address;
    picoquic_quic_t* quic = NULL;
    picoquic_cnx_t* cnx = NULL;
    sample_client_ctx_t client_ctx = { 0 };
   

    ret = sample_client_init(server_name, server_port, default_dir,
        &server_address, &quic, &cnx, &client_ctx);

    if (ret == 0) {
        /* Initialize all the streams contexts from the list of streams passed on the API. */
      

        /* Create a stream context for all the files that should be downloaded */
        //for (int i = 0; ret == 0 && i < client_ctx.nb_files; i++) {
            ret = sample_client_create_stream(cnx, &client_ctx);
            if (ret < 0) {
                fprintf(stderr, "Could not initiate stream for fi\n");
            }
        //}
    }

    /* Wait for packets */
    ret = picoquic_packet_loop(quic, 0, server_address.ss_family, 0, 0, 0, sample_client_loop_cb, &client_ctx);

    /* Done. At this stage, we could print out statistics, etc. */
    sample_client_report(&client_ctx);

    /* Save tickets and tokens, and free the QUIC context */
    if (quic != NULL) {
        picoquic_free(quic);
    }

    /* Free the Client context */
    sample_client_free_context(&client_ctx);

        receiveFile();
    }
};

class StitcherHelper {
private:
    string res;
    TileDims tiles;

public:
    StitcherHelper(string resolution) {
        res = resolution;
        tiles = TileDims();
    }

    void stitchFrame(Mat& source_frame, Mat& target_frame, vector<string>& tiles_str) {
        for (int i = 0; i < tiles_str.size(); i++) {
            int tile_index = stoi(tiles_str[i]);
            //cout<<tile_index<<endl;

            vector<int> dim = tiles.getTileDims(res, tile_index);

            // // Print the elements of the dim vector
            // cout << "Dimensions for tile " << tile_index << ": ";
            // for (int j = 0; j < dim.size(); j++) {
            //     cout << dim[j] << " ";
            // }
            // cout << endl;


            source_frame(Rect(dim[0], dim[1], dim[2] - dim[0], dim[3] - dim[1])).copyTo(
                target_frame(Rect(dim[0], dim[1], dim[2] - dim[0], dim[3] - dim[1])));
        }
    }
};

class PlayerHelper {
private:
    EvictingQueue& primaryQueue;
    EvictingQueue& helperQueue;
    StitcherHelper stitcher;
    std::vector<std::string> previousBackground;
    std::vector<std::future<void>> results;
    Stats stats;
    long lastRenderTime;
    //std::string logFile;

public:
    PlayerHelper(const std::string& resolution, EvictingQueue& pQueue, EvictingQueue& hQueue)
        : primaryQueue(pQueue), helperQueue(hQueue), stitcher(resolution) {}

    void play() {
        cout<<"I WOKE UP"<<endl;
        for(int j=0;j<iterations;j++) {
           // bool onlyFG = false;
            std::vector<std::string> helperStrings, primaryStrings;
            try {
                // while (true) {
                //     std::string primaryString = primaryQueue.take();
                //     if (!primaryString.empty()) {
                //         primaryStrings = split(primaryString, '_');
                //         break;
                //     }
                // }


               
                std::string primaryString = primaryQueue.take();
                std::string helperString = helperQueue.take();
                // Print the values of primaryString and helperString for debugging
                std::cout << "Primary string: " << primaryString << std::endl;
                std::cout << "Helper string: " << helperString << std::endl;

                std::string fgFileName=primaryString;
                 std::string bgFileName= helperString;
                 cout<<fgFileName<<"---------------"<<bgFileName<<endl;


                std::vector<std::string> t{"9","10","11","12","13","14","15"};
                // int fgFileIndex = -1, bgFileIndex = -1;
    

            // if (!helperString.empty()) {
            //     // Renders with previous BG if available
            //     helperStrings = split(helperString, '_');
            //     while (true) {
            //         // Synchronizing FG and BG Tiles
            //         fgFileIndex = std::stoi(primaryStrings[2]);
            //         bgFileIndex = std::stoi(helperStrings[2]);
            //         std::cout << "Synchronizing: " << fgFileIndex << " " << bgFileIndex << std::endl;
            //         if (fgFileIndex > bgFileIndex) {
            //             if (helperQueue.size() > 0) {
            //                 std::cout << "Adjusted for Lag" << std::endl;
            //                 helperStrings = split(helperQueue.take(), '_');
            //                 previousBackground = helperStrings;
            //             } else {
            //                 std::cout << "Can't adjusted for Lag" << std::endl;
            //                 previousBackground = helperStrings;
            //                 break;
            //             }
            //         } else if (fgFileIndex < bgFileIndex) {
            //             if (fgFileIndex > 1) {
            //                 std::vector<std::string> tmp = helperStrings;
            //                 helperStrings = previousBackground;
            //                 previousBackground = tmp;
            //                 std::cout << "Adjusted for Lead" << std::endl;
            //                 break;
            //             } else {
            //                 previousBackground = helperStrings;
            //                 std::cout << "Can't adjusted for Lead" << std::endl;
            //                 break;
            //             }
            //         } else {
            //             previousBackground = helperStrings;
            //             break;
            //         }
            //     }


            //     fgFileName = primaryStrings[0];
            //     bgFileName = helperStrings[0];
            //     t = split(helperStrings[1], ',');
            //     fgFileIndex = std::stoi(primaryStrings[2]);
            //     bgFileIndex = std::stoi(helperStrings[2]);

            //     if (t.size() == 1 && t[0] == "00") {
            //         onlyFG = true;
            //     }
            //     std::cout << "BG available, rendering with it" << std::endl;
            // } else if (helperString.empty() && (results[0].wait_for(std::chrono::seconds(0)) == std::future_status::ready || results[1].wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
            //     // Any one channel is down. Render of all the tiles received over one path as FG
            //     fgFileName = primaryStrings[0];
            //     fgFileIndex = std::stoi(primaryStrings[2]);
            //     onlyFG = true;
            //     std::cout << "One channel down, rendering all tiles" << std::endl;
            // } else if (helperString.empty() && !previousBackground.empty()) {
            //     // If no current BG but has previous render it
            //     fgFileName = primaryStrings[0];
            //     bgFileName = previousBackground[0];
            //     t = split(previousBackground[1], ',');
            //     fgFileIndex = std::stoi(primaryStrings[2]);
            //     bgFileIndex = std::stoi(previousBackground[2]);

            //     if (t.size() == 1 && t[0] == "00") {
            //         onlyFG = true;
            //     }
            //     std::cout << "No BG, rendering with last one" << std::endl;
            // } else {
            //     // For the first time, if no BG available render only FG
            //     fgFileName = primaryStrings[0];
            //     fgFileIndex = std::stoi(primaryStrings[2]);
            //     onlyFG = true;
            //     std::cout << "Rendering only FG" << std::endl;
            // }

            if(!fgFileName.empty()){

            cv::VideoCapture captFg(fgFileName);
            if (!captFg.isOpened()) {
            std::cerr << "Error: Couldn't open video file:" << fgFileName<< std::endl;
            }
       
            cv::VideoCapture captBg;
            captBg = cv::VideoCapture(bgFileName);
            if (!captBg.isOpened()) {
            std::cerr << "Error: Couldn't open video file:" << bgFileName<< std::endl;
            }
           
            cv::Mat bgFrame;
            // if (!onlyFG) {
            //     captBg = cv::VideoCapture(bgFileName);
            //     bgFrame = cv::Mat();
            // }

            cv::Mat fgFrame;
            // stats.delayInFGAndBGSegments = fgFileIndex - bgFileIndex;
            // std::cout << "Rendering: FG=" << fgFileName << " BG=" << bgFileName << " Delay: " << stats.delayInFGAndBGSegments << std::endl;
            // long currentRenderTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            // std::string stall = "[SFB]: " + std::to_string(currentRenderTime - lastRenderTime) + " " + std::to_string(fgFileIndex) + " " + std::to_string(bgFileIndex) + "\n";
            // std::cout << "Stall FG BG: " << stall << std::endl;
            // std::ofstream logFileStream(logFile, std::ios::app);
            // if (logFileStream.is_open()) {
            //     logFileStream << stall;
            //     logFileStream.close();
            // }

            while (true) {
                
                bool retFg = captFg.read(fgFrame);
               // if (!onlyFG) {
                    bool retBg = captBg.read(bgFrame);
                     
                    if (retBg) {
                        
                        stitcher.stitchFrame(bgFrame, fgFrame, t);
                     }
               // }
                if (retFg) {
                    cout<<"Debug"<<endl;
                    cv::imshow("MainWindow", fgFrame);
                    cv::waitKey(20);
                } else {
                    cout<<"OUT OF THE LOOP"<<endl;
                    break;
                }
            }
            
            }
            else{
                //this_thread::sleep_for(chrono::milliseconds(500));
            }


           // lastRenderTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        } catch (const std::exception& e) {
            cout<<"this is error block"<<endl;
            std::cerr << e.what() << std::endl;
        }
    }
}

void run() {
    this_thread::sleep_for(chrono::milliseconds(1000));
    play();
}


private:
    std::vector<std::string> split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::istringstream iss(s);
        std::string token;
        while (std::getline(iss, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }
};

int main() {
    //cv::String networkLagFile = "src/main/java/org/example/Multipath_ours_400segs.txt";
    cv::String primaryFolderName = "/home/arani/Aruba/ReceiverVideos/primary/";
    cv::String helperFolderName  = "/home/arani/Aruba/ReceiverVideos/helper/";

    // cv::String serverIp = "192.168.226.1";
    // cv::String helperIp = "192.168.226.1";
     cv::String serverIp = "127.0.0.1";
    cv::String helperIp = "127.0.0.1";
    int serverPort = 1221;
    int helperPort = 1222;
    int rttHandlerPortPrimary = 8001;
    int rttHandlerPortHelper = 8002;
    int controlChannelHandlerPortPrimary = 8003;
    int controlChannelHandlerPortHelper = 8004;
    int timeoutOfRttHi = 2000;
    int evictingQueuesPollWaitTime = 1000;
    Stats stats;

   

    //Cleaning older files
    try {
        system("bash clean.sh");
    } catch (exception& e) {
        cerr << e.what() << endl;
    }

    // Receiving Queues to share file names between receiving and stitching threads
    EvictingQueue primaryQueue(2, evictingQueuesPollWaitTime);
    EvictingQueue helperQueue(2, evictingQueuesPollWaitTime);
    vector<future<void>> results;

    // //////////////////// Control Channels //////////////////////////////////
    // // For RTT
    // RTTHandler rttHandlerPrimary(serverIp, rttHandlerPortPrimary, timeoutOfRttHi, 0);
    // RTTHandler rttHandlerHelper(helperIp, rttHandlerPortHelper, timeoutOfRttHi, 1);
    
    // // Start RTT threads
    // std::thread rttPrimaryThread(&RTTHandler::run, &rttHandlerPrimary);
    // std::thread rttHelperThread(&RTTHandler::run, &rttHandlerHelper);

    // // For bandwidth and chunk completion time
    ControlChannelHandler controlChannelHandlerPrimary(serverIp, controlChannelHandlerPortPrimary, 0);
    ControlChannelHandler controlChannelHandlerHelper(serverIp, controlChannelHandlerPortHelper, 1);


    ////////////////////// Video Channels ////////////////////////////////////
    Receiver helperRecv(helperIp, helperPort, primaryFolderName, helperFolderName, 1, primaryQueue, helperQueue, controlChannelHandlerHelper);
    Receiver primaryRecv(serverIp, serverPort, primaryFolderName, helperFolderName, 0, primaryQueue, helperQueue, controlChannelHandlerPrimary);
    
    // // // //vector<Receiver> receivers = {primaryRecv, helperRecv};

    // PlayerHelper player("1280x720", primaryQueue, helperQueue);
    
    // // // // Start receiver and player threads
    
    std::thread helperRecvThread(&Receiver::run, &helperRecv);
    std::thread primaryRecvThread(&Receiver::run, &primaryRecv);
    // std::thread playerThread(&PlayerHelper::run, &player);

    helperRecvThread.join();
    primaryRecvThread.join();
    // playerThread.join();


    // rttPrimaryThread.join();
    // rttHelperThread.join();


    cout<<"we reached the end of the program"<<endl;

    return 0;
}
