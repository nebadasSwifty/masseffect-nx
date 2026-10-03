#include "me_fragcoord_xy_spirv.h"
#include "me_depth_spirv.h"
#include "me_depth_quantize_spirv.h"
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Words=std::vector<uint32_t>;
using Mode=me::native::GuestFragCoordXYMode;
static void Check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
static void Emit(Words& w,uint32_t op,std::initializer_list<uint32_t> a){w.push_back((uint32_t(a.size()+1)<<16)|op);w.insert(w.end(),a);}
// FragCoord ID3 intentionally collides with W literal index3.
static Words Fixture(bool components=false,bool helper=false,bool absent=false){
 Words w{0x07230203,0x00010300,0,100,0};
 Emit(w,17,{1});Emit(w,14,{0,1});
 if(absent)Emit(w,15,{4,15,0x6E69616D,0,14});else Emit(w,15,{4,15,0x6E69616D,0,3,14});
 Emit(w,16,{15,7});if(!absent)Emit(w,71,{3,11,15});Emit(w,71,{14,30,0});
 Emit(w,19,{1});Emit(w,33,{2,1});Emit(w,22,{4,32});Emit(w,23,{5,4,4});
 Emit(w,32,{6,1,5});Emit(w,32,{7,3,5});Emit(w,32,{8,1,4});Emit(w,21,{9,32,1});
 for(uint32_t i=0;i<4;++i)Emit(w,43,{9,10+i,i});
 Emit(w,33,{16,5});Emit(w,43,{4,17,0});Emit(w,44,{5,18,17,17,17,17});
 if(!absent)Emit(w,59,{6,3,1});Emit(w,59,{7,14,3});
 Emit(w,54,{1,15,0,2});Emit(w,248,{20});
 if(absent)Emit(w,62,{14,18});
 else if(helper){Emit(w,57,{5,21,50});Emit(w,62,{14,21});}
 else if(components){
  for(uint32_t i=0;i<4;++i){Emit(w,65,{8,21+i,3,10+i});Emit(w,61,{4,31+i,21+i});}
  Emit(w,80,{5,40,31,32,33,34});Emit(w,62,{14,40});
 }else{Emit(w,61,{5,21,3});Emit(w,81,{4,22,21,3});Emit(w,62,{14,21});}
 Emit(w,253,{});Emit(w,56,{});
 if(helper){Emit(w,54,{5,50,0,16});Emit(w,248,{51});Emit(w,61,{5,52,3});Emit(w,254,{52});Emit(w,56,{});}
 return w;
}
static void Write(const std::string& p,const Words& w){Check(!std::filesystem::exists(p),"output exists");std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(w.data()),w.size()*4);Check(bool(f),"write failed");}
static Words Read(const char* p){std::ifstream f(p,std::ios::binary|std::ios::ate);auto n=f.tellg();Check(n>=20&&n%4==0,"input framing");Words w(size_t(n)/4);f.seekg(0);f.read(reinterpret_cast<char*>(w.data()),n);Check(bool(f),"read failed");return w;}
// Narrow interpreter for emitted load/arithmetic/reconstruction operations,
// not general SPIR-V execution. Z/W include payload NaN and negative zero.
static void Values(const Words& w,Mode m){
 using Value=std::array<uint32_t,4>;const Value raw{0x42F68000,0x42690000,0x7FC12345,0x80000000};
 std::map<uint32_t,Value> v;std::map<uint32_t,uint32_t> pointers{{3,4}};Value last{};bool observed=false;
 for(size_t at=5;at<w.size();at+=w[at]>>16){const auto* p=w.data()+at;uint32_t op=p[0]&65535;
  if(op==43)v[p[2]]={p[3],0,0,0};
  if(op==65)pointers[p[2]]=v.at(p[4])[0];
  if(op==61&&pointers.count(p[3]))v[p[2]]=pointers[p[3]]==4?raw:Value{raw[pointers[p[3]]],0,0,0};
  if(op==81&&v.count(p[3]))v[p[2]]={v[p[3]][p[4]],0,0,0};
  if((op==133||op==131)&&v.count(p[3])&&v.count(p[4])){float a=std::bit_cast<float>(v[p[3]][0]),b=std::bit_cast<float>(v[p[4]][0]);v[p[2]]={std::bit_cast<uint32_t>(op==133?a*b:a-b),0,0,0};}
  if(op==82&&v.count(p[4])){v[p[2]]=v[p[4]];v[p[2]][p[5]]=v.at(p[3])[0];}
  if(op==80){v[p[2]]={v.at(p[3])[0],v.at(p[4])[0],v.at(p[5])[0],v.at(p[6])[0]};}
  if((op==62&&p[1]==14&&v.count(p[2]))||(op==254&&v.count(p[1]))){last=v.at(p[op==62?2:1]);observed=true;}
 }
 Check(observed,"no evaluated result");float x=std::bit_cast<float>(raw[0]),y=std::bit_cast<float>(raw[1]);
 float gx=m==Mode::GridX2?x*.5f:x-.25f,gy=m==Mode::GridX2?y:y-(m==Mode::Phase1?.25f:-.25f);
 Check(last[0]==std::bit_cast<uint32_t>(gx)&&last[1]==std::bit_cast<uint32_t>(gy),"XY inverse mismatch");
 Check(last[2]==raw[2]&&last[3]==raw[3],"ZW payload changed");
}
int main(int argc,char** argv){try{
 if(argc==5&&(std::string(argv[1])=="--transform"||std::string(argv[1])=="--compose")){
  auto w=Read(argv[3]);Words out;std::string reason;
  Check(me::native::TransformGuestFragCoordXY(w,out,reason,Mode(std::stoul(argv[2]))),reason.c_str());
  if(std::string(argv[1])=="--compose"){
   Words half,quant;Check(me::native::TransformDepthHalf(out,half,reason),reason.c_str());
   Check(me::native::TransformDepthQuantizeIncoming(half,quant,reason,true),reason.c_str());
   uint32_t coord=0;bool explicit_depth=false;
   for(size_t at=5;at<half.size();at+=half[at]>>16)if((half[at]&65535)==71&&(half[at]>>16)==4&&half[at+2]==11){
    if(half[at+3]==15)coord=half[at+1];if(half[at+3]==22)explicit_depth=true;
   }
   unsigned raw_z_loads=0;
   for(size_t at=5;at<quant.size();at+=quant[at]>>16)if((quant[at]&65535)==61&&quant[at+3]==coord&&quant[at+2]>=half[3]){
    size_t next=at+(quant[at]>>16);
    Check(next+5<=quant.size()&&(quant[next]&65535)==81&&quant[next+3]==quant[at+2]&&quant[next+4]==2,
          "injected raw depth load was remapped as XY");++raw_z_loads;
   }
   if(coord&&!explicit_depth)Check(raw_z_loads>0,"missing raw injected depth load");
   out=std::move(quant);
  }
  Write(argv[4],out);return 0;
 }
 Check(argc==2,"usage: test OUTPUT_FRESH_DIRECTORY or --transform MODE INPUT OUTPUT");std::filesystem::create_directory(argv[1]);
 std::array<Words,3> phase;
 for(uint32_t mode=1;mode<=3;++mode)for(unsigned kind=0;kind<4;++kind){
  auto w=Fixture(kind==1,kind==2,kind==3);Words out;std::string reason;
  Check(me::native::TransformGuestFragCoordXY(w,out,reason,Mode(mode)),reason.c_str());
  if(kind==3)Check(out==w,"noFragCoord not exact passthrough");else Values(out,Mode(mode));
  if(kind==0)phase[mode-1]=out;
  std::string stem=std::string(argv[1])+"/mode"+std::to_string(mode)+"-kind"+std::to_string(kind);
  Write(stem+"-input.spv",w);Write(stem+".spv",out);
 }
 Check(phase[0]!=phase[1]&&phase[1]!=phase[2],"module identities collide");
 // FragCoord ID7 must not collide with Function StorageClass literal7.
 {auto w=Fixture();
  // Exchange IDs3 and7 ONLY at typed ID positions. Here7 is Output pointer
  // type, not used as an operand literal in this fixture.
  for(size_t at=5;at<w.size();at+=w[at]>>16){unsigned op=w[at]&65535,n=w[at]>>16;
   for(unsigned k=1;k<n;++k){bool literal=(op==17)||(op==14)||(op==15&&(k==1||k==3||k==4))||
    (op==16&&k>=2)||(op==71&&k>=2)||(op==22&&k==2)||(op==23&&k==3)||
    (op==32&&k==2)||(op==21&&k>=2)||(op==43&&k==3)||(op==59&&k==3)||
    (op==54&&k==3)||(op==81&&k>=4);
    if(!literal){if(w[at+k]==3)w[at+k]=7;else if(w[at+k]==7)w[at+k]=3;}}
  }
  size_t label=5;while((w[label]&65535)!=248)label+=w[label]>>16;label+=w[label]>>16;
  w.insert(w.begin()+label,{(4u<<16)|59,3,90,7}); // unused local Output pointer type invalid storage!
  // Make its type a genuine Function pointer to float4.
  size_t fn=5;while((w[fn]&65535)!=54)fn+=w[fn]>>16;
  w.insert(w.begin()+fn,{(4u<<16)|32,91,7,5});
  for(size_t at=5;at<w.size();at+=w[at]>>16)if((w[at]&65535)==59&&w[at+2]==90)w[at+1]=91;
  Words out;std::string reason;Check(me::native::TransformGuestFragCoordXY(w,out,reason,Mode::Phase2),reason.c_str());
  Write(std::string(argv[1])+"/id7-local-input.spv",w);Write(std::string(argv[1])+"/id7-local.spv",out);
  for(size_t at=5;at<w.size();at+=w[at]>>16)if((w[at]&65535)==59&&w[at+2]==90){w.insert(w.begin()+at+4,7);w[at]=(5u<<16)|59;break;}
  Check(!me::native::TransformGuestFragCoordXY(w,out,reason,Mode::Phase2)&&out.empty(),"initializer pointer escape accepted");
 }
 for(unsigned negative=0;negative<5;++negative){auto w=Fixture(true);Words out;std::string reason;
  if(negative==0){for(size_t at=5;at<w.size();at+=w[at]>>16)if((w[at]&65535)==65){w[at+4]=34;break;}}
  if(negative==1){for(size_t at=5;at<w.size();at+=w[at]>>16)if((w[at]&65535)==61){w[at]=(4u<<16)|124;break;}}
  if(negative==2)w.insert(w.begin()+5,{(5u<<16)|72,5,0,11,15});
  if(negative==3)w.back()=0;
  Check(!me::native::TransformGuestFragCoordXY(w,out,reason,negative==4?Mode(99):Mode::Phase1)&&out.empty(),"unsafe profile accepted");
 }
 auto w=Fixture();std::string reason;Check(!me::native::TransformGuestFragCoordXY(w,w,reason,Mode::GridX2),"alias accepted");
 std::cout<<"PASS 13 valid fixtures: exact XY/Z/W, helper loads, constant components, ID3/ID7 literal collisions, noFragCoord; 7 rejection guards\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
