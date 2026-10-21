#include "../../src/asr/text_metrics.hpp"
#include <stdexcept>
#include <iostream>
int main(){using namespace voice_engine::asr_metrics;
if(normalize("你好，世界！ USB ＡＢＣ。")!="你好世界usbabc"||edit_distance("你好世界","你好世间")!=1||edit_distance("","你好")!=2||edit_distance("打开灯","打开台灯")!=1)throw std::runtime_error("UTF8 CER/normalization failed");
std::cout<<"ASR_METRICS_OK\n";
}
