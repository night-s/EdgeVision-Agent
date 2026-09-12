#include "hardware/rknpu_infer/RKNNEngine.h"
#include "hardware/rknpu_infer/Preprocessor.h"
#include "third_party/nlohmann_json.hpp"
#include <fstream>
#include <iostream>
#include <chrono>

using json=nlohmann::json;
static json encode(const InferenceResult& result) {
 json rows=json::array();
 for(const auto& d:result.detections)
  rows.push_back({{"class_id",d.class_id},{"confidence",d.confidence},
    {"box",{d.box.x,d.box.y,d.box.width,d.box.height}}});
 return rows;
}
int main(int argc,char** argv) {
 try {
  if(argc!=4)throw std::runtime_error("Usage: edge_model_validation model.rknn frame-640x480.yuyv output.json");
  std::vector<uint8_t> raw(640*480*2);
  std::ifstream file(argv[2],std::ios::binary);
  if(!file.read(reinterpret_cast<char*>(raw.data()),raw.size()) || file.peek()!=EOF)
   throw std::runtime_error("requires exactly one packed 640x480 YUYV frame");
  Preprocessor cpu(640,480,false);
  cv::Mat rgb=cpu.run(raw.data()).clone();
  RKNNEngine engine;engine.loadModel(argv[1]);
  json report;report["input"]=argv[2];
  // The probability mode must fail for a raw-logit export.
  try {
   engine.infer(rgb,cpu.geometry(),.35,.45,false,true);
   report["activation"]="probabilities";
  }catch(const std::exception& e) {
   report["probability_mode_error"]=e.what();report["activation"]="logits";
  }
  bool logits=report["activation"]=="logits";
  double float_ms=0,native_ms=0;
  for(int i=0;i<12;++i){
   auto fp=engine.infer(rgb,cpu.geometry(),.35,.45,logits,false);
   auto quant=engine.infer(rgb,cpu.geometry(),.35,.45,logits,true);
   if(fp.detections.size()!=quant.detections.size())throw std::runtime_error("float/native detection count mismatch");
   for(size_t k=0;k<fp.detections.size();++k){
    const auto& a=fp.detections[k];const auto& b=quant.detections[k];
    if(a.class_id!=b.class_id || a.box!=b.box || std::abs(a.confidence-b.confidence)>1e-5)
     throw std::runtime_error("float/native detections differ");
   }
   if(i>=2){float_ms+=fp.output_ms+fp.post_ms;native_ms+=quant.output_ms+quant.post_ms;}
   report["cpu_detections"]=encode(quant);
  }
  report["float_native_match"]=true;
  report["samples_after_warmup"]=10;
  report["float_output_and_post_ms"]=float_ms/10;
  report["native_output_and_post_ms"]=native_ms/10;
#ifdef EDGE_WITH_RGA
  try {
   Preprocessor rga(640,480,true);
   auto accelerated=rga.run(raw.data()).clone();
   cv::Mat diff;cv::absdiff(rgb,accelerated,diff);
   auto mean=cv::mean(diff);double maximum=0;cv::minMaxLoc(diff.reshape(1),nullptr,&maximum);
   report["rga_rgb_mean_abs_error"]=(mean[0]+mean[1]+mean[2])/3.;
   report["rga_rgb_max_abs_error"]=maximum;
   auto result=engine.infer(accelerated,rga.geometry(),.35,.45,logits,true);
   report["rga_detections"]=encode(result);
   double cpu_ms=0,rga_ms=0;
   for(int i=0;i<50;++i){
    auto a=std::chrono::steady_clock::now();cpu.run(raw.data());auto b=std::chrono::steady_clock::now();
    rga.run(raw.data());auto c=std::chrono::steady_clock::now();
    cpu_ms+=std::chrono::duration<double,std::milli>(b-a).count();
    rga_ms+=std::chrono::duration<double,std::milli>(c-b).count();
   }
   report["cpu_preprocess_ms"]=cpu_ms/50;report["rga_preprocess_ms"]=rga_ms/50;
   report["rga_available"]=true;
  }catch(const std::exception& e){report["rga_available"]=false;report["rga_error"]=e.what();}
#endif
  std::ofstream out(argv[3]);out<<report.dump(2)<<"\n";
  if(!out)throw std::runtime_error("cannot write validation report");
  std::cout<<report.dump(2)<<"\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
