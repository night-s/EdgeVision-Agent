#include "app/VisionPipeline.h"
#include <sys/signalfd.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <iostream>
int main(int argc,char** argv){
 if(argc>2 || (argc==2 && std::string(argv[1])=="--help")){
  std::cout<<"Usage: edge_agent [config.json]\nDefault: configs/default.json\n";return argc>2?1:0;
 }
 sigset_t signals;sigemptyset(&signals);sigaddset(&signals,SIGINT);sigaddset(&signals,SIGTERM);
 if(pthread_sigmask(SIG_BLOCK,&signals,nullptr)!=0)return 1;
 int signal_fd=signalfd(-1,&signals,SFD_NONBLOCK|SFD_CLOEXEC);
 if(signal_fd<0){std::cerr<<"signalfd failed\n";return 1;}
 int result=0;
 try{
  auto config=Config::load(argc==2?argv[1]:"configs/default.json");
  VisionPipeline pipeline(config);pipeline.start();
  auto start=FrameClock::now();
  while(pipeline.running()){
   pollfd p{signal_fd,POLLIN,0};if(poll(&p,1,200)>0)break;
   if(config.run_seconds && FrameClock::now()-start>=std::chrono::seconds(config.run_seconds))break;
  }
  pipeline.stop();std::cout<<"[shutdown] all workers stopped\n";
 }catch(const std::exception& e){std::cerr<<"[fatal] "<<e.what()<<std::endl;result=1;}
 ::close(signal_fd);return result;
}
