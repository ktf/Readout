///
/// @file    mainReadout.cxx
/// @author  Sylvain
///

#include <InfoLogger/InfoLogger.hxx>
#include <Common/Configuration.h>
#include <Common/DataBlock.h>
#include <Common/DataBlockContainer.h>
#include <Common/MemPool.h>
#include <Common/DataSet.h>
#include <Common/Timer.h>
#include <Common/Fifo.h>
#include <Common/Thread.h>

#ifdef WITH_CONFIG
#include <Configuration/ConfigurationFactory.h>
#endif


#include <atomic>
#include <chrono>
#include <memory>
#include <signal.h>
#include <vector>
#include <map>

#include "ReadoutEquipment.h"
#include "DataBlockAggregator.h"
#include "Consumer.h"
#include "MemoryBankManager.h"
#include "ReadoutUtils.h"


#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef WITH_NUMA
#include <numa.h>
#endif

// option to add callgrind instrumentation
// to use: valgrind --tool=callgrind --instr-atstart=no --dump-instr=yes ./a.out
// to display stats: kcachegrind
//#define CALLGRIND
#ifdef CALLGRIND
#include <valgrind/callgrind.h>
#endif


// option to enable compilation with FairMQ support
#ifdef WITH_FAIRMQ
#include <fairmq/FairMQLogger.h>
#include <InfoLogger/InfoLoggerFMQ.hxx>
#endif


// option to enable compilation with Control-OCCPlugin support
//#define WITH_OCC
//#undef WITH_OCC
#ifdef WITH_OCC
  #include <OccInstance.h>
  #include <RuntimeControlledObject.h>
#endif


// namespace used
using namespace AliceO2::InfoLogger;
using namespace AliceO2::Common;


// global entry point to log system
InfoLogger theLog;


// global signal handler to end program
static int ShutdownRequest=0;      // set to 1 to request termination, e.g. on SIGTERM/SIGQUIT signals
static void signalHandler(int signalId) {
  theLog.log("Received signal %d",signalId);
  printf("*** break ***\n");
  if (ShutdownRequest) {
    // immediate exit if pending exit request
    exit(1);
  }
  ShutdownRequest=1;
}


class Readout {
  
  public:
    
  int init(int argc, char* argv[]);  
  int configure();
  int reset(); // as opposed to configure()
  int start();
  int stop(); // as opposed to start()
  int iterateRunning();
  int iterateCheck();

  private:
  
  ConfigFile cfg;
  const char* cfgFileURI="";
  const char* cfgFileEntryPoint=""; // where in the config tree to look for
    
  // configuration parameters
  double cfgExitTimeout;
  double cfgFlushEquipmentTimeout;
  int cfgDisableAggregatorSlicing;
  
  //runtime entities
  std::vector<std::unique_ptr<Consumer>> dataConsumers;
  std::map<Consumer *, std::string> consumersOutput; // for the consumers having an output, keep a reference to the consumer and the name of the consumer to which to push data
  std::vector<std::unique_ptr<ReadoutEquipment>> readoutDevices;
  std::unique_ptr<DataBlockAggregator> agg;
  std::unique_ptr<AliceO2::Common::Fifo<DataSetReference>> agg_output;

  int isRunning=0; // set to 1 when running
  AliceO2::Common::Timer t; // time counter from start()

  int latencyFd=-1; // file descriptor for the "deep sleep" disabled

  bool isError=0; // flag set to 1 when error has been detected
  std::vector<std::string> strErrors; // errors backlog
  std::mutex mutexErrors; // mutex to guard access to error variables
};

int Readout::init(int argc, char* argv[]) {
  if (argc<2) {
    printf("Please provide path to configuration file\n");
    return -1;
  }
  cfgFileURI=argv[1];
  if (argc>2) {
    cfgFileEntryPoint=argv[2];
  }

  // configure signal handlers for clean exit
  struct sigaction signalSettings;
  bzero(&signalSettings,sizeof(signalSettings));
  signalSettings.sa_handler=signalHandler;
  sigaction(SIGTERM,&signalSettings,NULL);
  sigaction(SIGQUIT,&signalSettings,NULL);
  sigaction(SIGINT,&signalSettings,NULL);

  // log startup and options
  theLog.log("Readout process starting, pid %d",getpid());
  theLog.log("Optional built features enabled:");
  #ifdef WITH_FAIRMQ
    theLog.log("FAIRMQ : yes");
    // redirect FMQ logs to infologger
    setFMQLogsToInfoLogger(&theLog);
  #else
    theLog.log("FAIRMQ : no");
  #endif
  #ifdef WITH_NUMA
    theLog.log("NUMA : yes");
  #else
    theLog.log("NUMA : no");
  #endif
  #ifdef WITH_RDMA
    theLog.log("RDMA : yes");
  #else
    theLog.log("RDMA : no");
  #endif
  #ifdef WITH_OCC
    theLog.log("OCC : yes");
  #else
    theLog.log("OCC : no");
  #endif
  
  return 0;
}

int Readout::configure() {
  // load configuration file
  theLog.log("Reading configuration from %s %s",cfgFileURI,cfgFileEntryPoint);
  try {
    // check URI prefix
    if (!strncmp(cfgFileURI,"file:",5)) {
      // let's use the 'Common' config file library
      cfg.load(cfgFileURI);  
    } else {
      // otherwise use the Configuration module, if available
      #ifdef WITH_CONFIG
      try {
        std::unique_ptr<o2::configuration::ConfigurationInterface> conf = o2::configuration::ConfigurationFactory::getConfiguration(cfgFileURI);
        boost::property_tree::ptree t=conf->getRecursive(cfgFileEntryPoint);
        cfg.load(t);
        //cfg.print();
      }
      catch (std::exception &e) {
        throw std::string(e.what());
      }
      # else
        throw std::string("This type of URI is not supported");
      #endif
    }
  }
  catch (std::string err) {
    theLog.log("Error : %s",err.c_str());
    return -1;
  }


  // try to prevent deep sleeps  
  theLog.log("Disabling CPU deep sleep for process");
  int maxLatency=2;
  latencyFd = open("/dev/cpu_dma_latency", O_WRONLY);
  if (latencyFd < 0) {
    theLog.log("Error opening /dev/cpu_dma_latency");
  } else {
    if (write(latencyFd, &maxLatency, sizeof(maxLatency)) != sizeof(maxLatency)) {
      theLog.log("Error writing to /dev/cpu_dma_latency");
    }
  }
  

  // extract optional configuration parameters
  // configuration parameter: | readout | exitTimeout | double | -1 | Time in seconds after which the program exits automatically. -1 for unlimited. |
  cfgExitTimeout=-1;
  cfg.getOptionalValue<double>("readout.exitTimeout",cfgExitTimeout);
  // configuration parameter: | readout | flushEquipmentTimeout | double | 0 | Time in seconds to wait for data once the equipments are stopped. 0 means stop immediately. |
  cfgFlushEquipmentTimeout=0;
  cfg.getOptionalValue<double>("readout.flushEquipmentTimeout",cfgFlushEquipmentTimeout);
  // configuration parameter: | readout | disableAggregatorSlicing | int | 0 | When set, the aggregator slicing is disabled, data pages are passed through without grouping/slicing. |
  cfgDisableAggregatorSlicing=0;
  cfg.getOptionalValue<int>("readout.disableAggregatorSlicing",cfgDisableAggregatorSlicing);
  
  

  // configuration of memory banks
  int numaNodeChanged=0;
  for (auto kName : ConfigFileBrowser (&cfg,"bank-")) {
     // skip disabled
    int enabled=1;
    try {
      // configuration parameter: | bank-* | enabled | int | 1 | Enable (1) or disable (0) the memory bank. |
      enabled=cfg.getValue<int>(kName + ".enabled");
    }
    catch (...) {
    }
    if (!enabled) {continue;}

    // bank size
    // configuration parameter: | bank-* | size | bytes | | Size of the memory bank, in bytes. |
    std::string cfgSize="";
    cfg.getOptionalValue<std::string>(kName + ".size",cfgSize);
    long long mSize=ReadoutUtils::getNumberOfBytesFromString(cfgSize.c_str());
    if (mSize<=0) {
      theLog.log("Skipping memory bank %s:  wrong size %s",kName.c_str(),cfgSize.c_str());
      continue;
    }

    // bank type
    // configuration parameter: | bank-* | type | string| | Support used to allocate memory. Possible values: malloc, MemoryMappedFile. |
    std::string cfgType="";
    try {
      cfgType=cfg.getValue<std::string>(kName + ".type");
    }
    catch (...) {
      theLog.log("Skipping memory bank %s:  no type specified",kName.c_str());
      continue;
    }
    if (cfgType.length()==0) {continue;}

    // numa node
    // configuration parameter: | bank-* | numaNode | int | -1| Numa node where memory should be allocated. -1 means unspecified (system will choose). |
    int cfgNumaNode=-1;
    cfg.getOptionalValue<int>(kName + ".numaNode",cfgNumaNode);

    // instanciate new memory pool
    if (cfgNumaNode>=0) {
      #ifdef WITH_NUMA
      struct bitmask *nodemask;
      nodemask=numa_allocate_nodemask();
      if (nodemask==NULL) {return -1;}
      numa_bitmask_clearall(nodemask);
      numa_bitmask_setbit(nodemask,cfgNumaNode);
      numa_set_membind(nodemask);
      numa_free_nodemask(nodemask);
      theLog.log("Enforcing memory allocated on NUMA node %d",cfgNumaNode);
      numaNodeChanged=1;
      #endif
    }
    theLog.log("Creating memory bank %s: type %s size %lld",kName.c_str(),cfgType.c_str(),mSize);
    std::shared_ptr<MemoryBank> b=nullptr;
    try {
      b=getMemoryBank(mSize, cfgType, kName);
    }
    catch (...) {    
    }
    if (b==nullptr) {
      theLog.log(InfoLogger::Severity::Error,"Failed to create memory bank %s",kName.c_str());
      continue;
    }
    // cleanup the memory range
    b->clear();
    // add bank to list centrally managed
    theMemoryBankManager.addBank(b,kName);
    theLog.log("Bank %s added",kName.c_str());
  }
  
  // releasing memory bind policy
  if (numaNodeChanged){
    #ifdef WITH_NUMA
    struct bitmask *nodemask;
    nodemask=numa_get_mems_allowed();
    numa_set_membind(nodemask);
    // is this needed? not specified in doc...
    //numa_free_nodemask(nodemask);
    theLog.log("Releasing memory NUMA node enforcment");
    #endif
  }
  
  
  // configuration of data consumers
  for (auto kName : ConfigFileBrowser (&cfg,"consumer-")) {

    // skip disabled
    int enabled=1;
    try {
      // configuration parameter: | consumer-* | enabled | int | 1 | Enable (value=1) or disable (value=0) the consumer. |
      enabled=cfg.getValue<int>(kName + ".enabled");
    }
    catch (...) {
    }
    if (!enabled) {continue;}

    // configuration parameter: | consumer-* | consumerOutput | string |  | Name of the consumer where the output of this consumer (if any) should be pushed. |
    std::string cfgOutput="";
    cfg.getOptionalValue<std::string>(kName + ".consumerOutput",cfgOutput);

    // instanciate consumer of appropriate type
    std::unique_ptr<Consumer> newConsumer=nullptr;
    try {
      // configuration parameter: | consumer-* | consumerType | string |  | The type of consumer to be instanciated. One of:stats, FairMQDevice, DataSampling, FairMQChannel, fileRecorder, checker, processor, tcp. |
      std::string cfgType="";
      cfgType=cfg.getValue<std::string>(kName + ".consumerType");
      theLog.log("Configuring consumer %s: %s",kName.c_str(),cfgType.c_str());

      if (!cfgType.compare("stats")) {
        newConsumer=getUniqueConsumerStats(cfg, kName);
      } else if (!cfgType.compare("FairMQDevice")) {
        #ifdef WITH_FAIRMQ
          newConsumer=getUniqueConsumerFMQ(cfg, kName);
        #else
          theLog.log("Skipping %s: %s - not supported by this build", kName.c_str(), cfgType.c_str());
        #endif
      } else if (!cfgType.compare("DataSampling")) {
        #ifdef WITH_FAIRMQ
          newConsumer=getUniqueConsumerDataSampling(cfg, kName);
        #else
          theLog.log("Skipping %s: %s - not supported by this build",kName.c_str(),cfgType.c_str());
        #endif
      } else if (!cfgType.compare("FairMQChannel")) {
        #ifdef WITH_FAIRMQ
          newConsumer=getUniqueConsumerFMQchannel(cfg, kName);
        #else
          theLog.log("Skipping %s: %s - not supported by this build",kName.c_str(),cfgType.c_str());
        #endif
      } else if (!cfgType.compare("fileRecorder")) {
        newConsumer=getUniqueConsumerFileRecorder(cfg, kName);
      } else if (!cfgType.compare("checker")) {
        newConsumer=getUniqueConsumerDataChecker(cfg, kName);
      } else if (!cfgType.compare("processor")) {
        newConsumer=getUniqueConsumerDataProcessor(cfg, kName);
      } else if (!cfgType.compare("tcp")) {
        newConsumer=getUniqueConsumerTCP(cfg, kName);
      } else if (!cfgType.compare("rdma")) {
        #ifdef WITH_RDMA
	  newConsumer=getUniqueConsumerRDMA(cfg, kName);
	#else
	  theLog.log("Skipping %s: %s - not supported by this build", kName.c_str(), cfgType.c_str());
	#endif
      } else {
        theLog.log("Unknown consumer type '%s' for [%s]",cfgType.c_str(),kName.c_str());
      }
    }
    catch (const std::exception& ex) {
        theLog.log(InfoLogger::Severity::Error,"Failed to configure consumer %s : %s",kName.c_str(), ex.what());
        continue;
    }
    catch (const std::string& ex) {
        theLog.log(InfoLogger::Severity::Error,"Failed to configure consumer %s : %s",kName.c_str(), ex.c_str());
        continue;
    }
    catch (...) {
        theLog.log(InfoLogger::Severity::Error,"Failed to configure consumer %s",kName.c_str());
        continue;
    }

    if (newConsumer!=nullptr) {
      if (cfgOutput.length()>0) {
        consumersOutput.insert(std::pair<Consumer *, std::string>(newConsumer.get(),cfgOutput));        
      }
      newConsumer->name=kName;
      dataConsumers.push_back(std::move(newConsumer));
    }

  }
  
  // try to link consumers with output
  for(auto const &p : consumersOutput) {
    // search for consumer with this name
    bool found=false;
    std::string err="not found";
    for(auto const &c: dataConsumers) {
      if (c->name==p.second) {
        if (c->isForwardConsumer) {
          err="already used";
          break;
        }
        theLog.log("Output of %s will be pushed to %s",p.first->name.c_str(),c->name.c_str());
        found=true;
        c->isForwardConsumer=true;
        p.first->forwardConsumer=c.get();
        break;
      }
    }
    if (!found) {
      theLog.log(InfoLogger::Severity::Error,"Failed to attach consumer %s to %s (%s)",p.first->name.c_str(),p.second.c_str(),err.c_str());
    }
  }


  // configure readout equipments
  int nEquipmentFailures=0; // number of failed equipment instanciation  
  for (auto kName : ConfigFileBrowser (&cfg,"equipment-")) {

    // example iteration on each sub-key
    //for (auto kk : ConfigFileBrowser (&cfg,"",kName)) {
    //  printf("%s -> %s\n",kName.c_str(),kk.c_str());
    //}

    // skip disabled equipments
    // configuration parameter: | equipment-* | enabled | int | 1 | Enable (value=1) or disable (value=0) the equipment. |
    int enabled=1;
    cfg.getOptionalValue<int>(kName + ".enabled",enabled);
    if (!enabled) {continue;}

    // configuration parameter: | equipment-* | equipmentType | string |  | The type of equipment to be instanciated. One of: dummy, rorc, cruEmulator |
    std::string cfgEquipmentType="";
    cfgEquipmentType=cfg.getValue<std::string>(kName + ".equipmentType");
    theLog.log("Configuring equipment %s: %s",kName.c_str(),cfgEquipmentType.c_str());

    std::unique_ptr<ReadoutEquipment>newDevice=nullptr;
    try {
      if (!cfgEquipmentType.compare("dummy")) {
        newDevice=getReadoutEquipmentDummy(cfg,kName);
      } else if (!cfgEquipmentType.compare("rorc")) {
        newDevice=getReadoutEquipmentRORC(cfg,kName);
      } else if (!cfgEquipmentType.compare("cruEmulator")) {
        newDevice=getReadoutEquipmentCruEmulator(cfg,kName);
      } else if (!cfgEquipmentType.compare("player")) {
        newDevice=getReadoutEquipmentPlayer(cfg,kName);
      } else {
        theLog.log("Unknown equipment type '%s' for [%s]",cfgEquipmentType.c_str(),kName.c_str());
      }
    }
    catch (std::string errMsg) {
        theLog.log(InfoLogger::Severity::Error,"Failed to configure equipment %s : %s",kName.c_str(),errMsg.c_str());
        nEquipmentFailures++;
        continue;
    }
    catch (int errNo) {
        theLog.log(InfoLogger::Severity::Error,"Failed to configure equipment %s : error #%d",kName.c_str(),errNo);
        nEquipmentFailures++;
        continue;
    }
    catch (...) {
        theLog.log(InfoLogger::Severity::Error,"Failed to configure equipment %s",kName.c_str());
        nEquipmentFailures++;
        continue;
    }

    // add to list of equipments
    if (newDevice!=nullptr) {
      readoutDevices.push_back(std::move(newDevice));
    }
  }

  if (nEquipmentFailures) {
    theLog.log(InfoLogger::Severity::Fatal,"Some equipments failed to initialize, exiting");
    return -1;
  }


  // aggregator
  theLog.log("Creating aggregator");
  agg_output=std::make_unique<AliceO2::Common::Fifo<DataSetReference>>(1000);
  int nEquipmentsAggregated=0;
  agg=std::make_unique<DataBlockAggregator>(agg_output.get(),"Aggregator");
    
  for (auto && readoutDevice : readoutDevices) {
      //theLog.log("Adding equipment: %s",readoutDevice->getName().c_str());
      agg->addInput(readoutDevice->dataOut);
      nEquipmentsAggregated++;
  }
  theLog.log("Aggregator: %d equipments", nEquipmentsAggregated);
  return 0;
}

int Readout::start() {

  theLog.log("Starting aggregator");
  if (cfgDisableAggregatorSlicing) {
    theLog.log("Aggregator slicing disabled");
    agg->disableSlicing=1;
  }
  agg->start();

  // notify consumers of imminent data flow start
  for (auto& c : dataConsumers) {
    c->start();
  }

  theLog.log("Starting readout equipments");
  for (auto && readoutDevice : readoutDevices) {
      readoutDevice->start();
  }

  for (auto && readoutDevice : readoutDevices) {
      readoutDevice->setDataOn();
  }
  theLog.log("Running");

  // reset exit timeout, if any
  if (cfgExitTimeout>0) {
    t.reset(cfgExitTimeout*1000000);
    theLog.log("Automatic exit in %.2f seconds",cfgExitTimeout);
  }
  isRunning=1;

  theLog.log("Entering loop");
  #ifdef CALLGRIND
    theLog.log("Starting callgrind instrumentation");
    CALLGRIND_START_INSTRUMENTATION;
  #endif

  return 0;
}

int Readout::iterateCheck() {
  if (isError) {
    return -1;
  }
  return 0;
}

int Readout::iterateRunning() {
  if (isRunning) {
    if (((cfgExitTimeout>0)&&(t.isTimeout()))||(ShutdownRequest)) {
      isRunning=0;
      theLog.log("Stopping data readout");
      for (auto && readoutDevice : readoutDevices) {

	if (cfgFlushEquipmentTimeout<=0) {
	  theLog.log("Stopping immediately readout equipments, last pages might be lost");
	  // stop readout loop before stopping data (and loose the last pages)
          // otherwise we get incomplete pages of unkown size (driver bug), impossible to parse
	  readoutDevice->stop();
	}
        readoutDevice->setDataOff();
	// todo: should flush aggregator content after a while
      }

      t.reset(cfgFlushEquipmentTimeout*1000000);  // add a delay before stopping aggregator - continune to empty FIFOs
    }
  } else {
    if (t.isTimeout()) {
      return 1; // should stop running
    }
  }

  DataSetReference bc=nullptr;
  agg_output->pop(bc);


  if (bc!=nullptr) {
    for (auto& c : dataConsumers) {
      // push only to "prime" consumers, not to those getting data directly forwarded from another consumer
      if (c->isForwardConsumer==false) {
        c->pushData(bc);
      }
    }
  } else {
    usleep(1000);
  }

  return 0;
}

int Readout::stop() {
  #ifdef CALLGRIND
    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS;
    theLog.log("Stopping callgrind instrumentation");
  #endif

  for (auto && readoutDevice : readoutDevices) {
    readoutDevice->stop();
  }
  theLog.log("Readout stopped");
  	
  theLog.log("Stopping aggregator");
  agg->stop();

  theLog.log("Stopping consumers");
  // notify consumers of imminent data flow stop
  for (auto& c : dataConsumers) {
    c->stop();
  }

  // ensure output buffers empty ?
  return 0;
}

int Readout::reset() {

  // close consumers before closing readout equipments (owner of data blocks)
  theLog.log("primary consumers");
  for(int i=0;i<dataConsumers.size();i++) {
    if (!dataConsumers[i]->isForwardConsumer) {
      dataConsumers[i]=nullptr;
    }
  }
  theLog.log("secondary consumers");
  for(int i=0;i<dataConsumers.size();i++) {
    if (dataConsumers[i]!=nullptr) {
      dataConsumers[i]=nullptr;
    }
  }  
  dataConsumers.clear();

  agg_output->clear();
  agg=nullptr;  // destroy aggregator, and release blocks it may still own.
   
  // todo: check nothing in the input pipeline
  // flush & stop equipments
  for (auto && readoutDevice : readoutDevices) {
      // ensure nothing left in output FIFO to allow releasing memory
//      printf("readout: in=%llu  out=%llu\n",readoutDevice->dataOut->getNumberIn(),readoutDevice->dataOut->getNumberOut());
      readoutDevice->dataOut->clear();
  }

//  printf("agg: in=%llu  out=%llu\n",agg_output.getNumberIn(),agg_output.getNumberOut());

  theLog.log("Closing readout devices");
  for (size_t i = 0, size = readoutDevices.size(); i != size; ++i) {
    readoutDevices[i]=nullptr;  // effectively deletes the device
  }
  //readoutDevices.clear(); // to do it all in one go

  if (latencyFd>=0) {
    close(latencyFd);
    latencyFd=-1;
  }

  theLog.log("Operations completed");
  return 0;
}


#ifdef WITH_OCC
class ReadoutOCCStateMachine : public RuntimeControlledObject
{
public:
  ReadoutOCCStateMachine(Readout *r) : RuntimeControlledObject("Readout Process") {
    theReadout=r;
  }

  int executeConfigure(const PropertyMap& properties){
    return theReadout->configure();
  }
  
  int executeReset() {
    return theReadout->reset();
  }
  
  int executeRecover() {
    return -1;
  }
  
  int executeStart() {
    return theReadout->start();
  }
  
  int executeStop() {
    return theReadout->stop();
  }
  
  int executePause() {
    return -1;
  }
  
  int executeResume() {
    return -1;
  }

  int executeExit() {
    return -1;
  }

  int iterateRunning() {
    return theReadout->iterateRunning();
  }
  
  int iterateCheck() {
    return theReadout->iterateCheck();
  }
  
private:
  Readout *theReadout=nullptr;  
};
#endif




// the main program loop
int main(int argc, char* argv[])
{
  Readout theReadout;
  
  int err=0;

  err=theReadout.init(argc,argv);
  if (err) {
    return err;
  }

#ifdef WITH_OCC

  // start OCC - control
  ReadoutOCCStateMachine csm(&theReadout);
  OccInstance occ(&csm);

  for (;;) {
    sleep(1);
    if (ShutdownRequest) {
      break;
    }
  }
  occ.wait();

/*
  // stop OCC - control
  stopOcc();
  return 0;
}
*/


#else
  
  err=theReadout.configure();
  if (err) {
    return err;
  }

  // loop for testing, single iteration in normal conditions
  for (int i=0;i<1;i++) {
  
  err=theReadout.start();
  if (err) {
    return err;
  }
  while (1) {
    err=theReadout.iterateRunning();
    if (err) break;
    err=theReadout.iterateCheck();
    if (err) break;
  }
  err=theReadout.stop();
  if (err) {
    return err;
  }
  
  sleep(3);
  }
  
#endif
 
  return 0;
}
