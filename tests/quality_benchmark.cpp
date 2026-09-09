// Engineering-only comparison against human/toolkit annotated PHN references.
// This executable is never installed or called by the release application.
#include "engine.hpp"
#include "phn.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <numeric>
namespace fs=std::filesystem;
int main(int argc,char**argv){
 fs::path scratch;
 try {
  if(argc!=3)throw std::runtime_error("Usage: neolip_quality_benchmark <NeoLIP source> <manifest.tsv> (audio, transcript, reference.phn; paths relative to manifest)");
  const auto root=fs::u8path(argv[1]),manifest=fs::absolute(fs::u8path(argv[2]));
  scratch=fs::temp_directory_path()/fs::u8path("NeoLIP quality benchmark "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(scratch);
  neolip::Engine engine({root/"third_party/cslu_runtime/Toolkit/2.0/apps/fa",root/"dictionaries",{}});
  std::ifstream in(manifest);if(!in)throw std::runtime_error("Cannot open benchmark manifest");
  std::cout<<"audio\tstatus\tnatural_keys\tminimal_keys\treview_flags\tmatched_phone_intervals\tboundary_mae_ms\tboundary_max_ms\talignment_ms\tpreset_preview_ms\n";
  std::string line;std::size_t index=0;bool failed=false;
  while(std::getline(in,line)){
   if(line.empty()||line.front()=='#')continue;
   if(line.back()=='\r')line.pop_back();std::istringstream row(line);std::string audio,text,reference;
   if(!std::getline(row,audio,'\t')||!std::getline(row,text,'\t')||!std::getline(row,reference,'\t'))throw std::runtime_error("Manifest needs three tab-separated paths");
   const auto resolve=[&](const std::string&p){const auto path=fs::u8path(p);return path.is_absolute()?path:manifest.parent_path()/path;};
   neolip::CreateRequest request{resolve(audio),neolip::readTranscript(resolve(text)),scratch/(std::to_string(index++)+".lip")};
   const auto start=std::chrono::steady_clock::now();auto a=engine.create(request);const auto aligned=std::chrono::steady_clock::now();
   if(a.status!=neolip::Status::Created||!a.prepared){failed=true;std::cout<<audio<<"\tFAILED: "<<a.message<<'\n';continue;}
   request.movement=neolip::MouthMovement::MinimalSmoothing;auto b=engine.preview(request);const auto end=std::chrono::steady_clock::now();
   if(b.status!=neolip::Status::Previewed||!b.reusedAlignment)throw std::runtime_error("Preset comparison did not reuse alignment");
   const auto ref=cslu::readPhn(resolve(reference));const auto&got=a.prepared->phonemes.intervals;
   bool same=ref.intervals.size()==got.size();for(std::size_t i=0;same&&i<got.size();++i)same=ref.intervals[i].label==got[i].label;
   std::vector<double>errors;if(same)for(std::size_t i=0;i<got.size();++i){errors.push_back(std::abs(double(got[i].startMs-ref.intervals[i].startMs)));errors.push_back(std::abs(double(got[i].endMs-ref.intervals[i].endMs)));}
   std::cout<<audio<<'\t'<<(same?"comparable":"different_phone_sequence")<<'\t'<<a.mouthKeys<<'\t'<<b.mouthKeys<<'\t'<<a.warnings.size()<<'\t'<<(same?got.size():0)<<'\t';
   if(errors.empty())std::cout<<"NA\tNA";else std::cout<<std::accumulate(errors.begin(),errors.end(),0.0)/errors.size()<<'\t'<<*std::max_element(errors.begin(),errors.end());
   std::cout<<'\t'<<std::chrono::duration<double,std::milli>(aligned-start).count()<<'\t'<<std::chrono::duration<double,std::milli>(end-aligned).count()<<'\n';
  }
  fs::remove_all(scratch);return failed?1:0;
 }catch(const std::exception&e){std::error_code ec;if(!scratch.empty())fs::remove_all(scratch,ec);std::cerr<<e.what()<<'\n';return 1;}
}
