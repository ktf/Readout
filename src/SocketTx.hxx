#include <thread>
#include <memory>
#include <atomic>
#include <Common/Timer.h>

#include <Common/DataBlock.h>
#include <Common/DataBlockContainer.h>
#include <Common/DataSet.h>
#include <Common/Fifo.h>

// class to send data blocks remotely over a TCP/IP socket
class SocketTx {
  public:

  // constructor
  // name: name given to this client, for logging purpose
  // serverHost: IP of remote server to connect to
  // serverPort: port number of remote server to connect to
  SocketTx(std::string name, std::string serverHost, int serverPort);
  
  // destructor
  ~SocketTx();

  // push a new piece of data to output socket
  // returns 0 on success, or -1 on error (e.g. when already busy sending something)
  int pushData(DataBlockContainerReference &b);

  private:
  std::unique_ptr<std::thread> th;	// thread pushing data to socket
  std::atomic<int> shutdownRequest;	// flag to be set to 1 to stop thread

  // todo: create an input queue, instead of single block
  //std::unique_ptr<AliceO2::Common::Fifo<DataBlockContainerReference>> blocks;

  unsigned long long bytesTx=0;	// number of bytes sent
  AliceO2::Common::Timer t; // timer, to count active time
  std::string clientName; // name of client, as given to constructor

  std::string serverHost; // remote server IP
  int serverPort; // remote server port
  
  private:  
  std::atomic<int> isSending; // if set, thread busy sending. if not set, new block can be pushed
  DataBlockContainerReference currentBlock=nullptr; // current data chunk being sent
  size_t currentBlockIndex=0; // number of bytes of chunk already sent
  
  void run(); // code executed in separate thread
};
