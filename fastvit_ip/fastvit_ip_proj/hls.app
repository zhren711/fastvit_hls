<AutoPilot:project xmlns:AutoPilot="com.autoesl.autopilot.project" projectType="C/C++" name="fastvit_ip_proj" ideType="classic" top="fastvit_ip">
    <files>
        <file name="gelu_worker.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="add_worker.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="pwconv_worker.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="dwconv_worker.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="conv_worker.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="fastvit_ip.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="../../fastvit_ip_tb.cpp" sc="0" tb="1" cflags="-std=c++14 -Wno-unknown-pragmas" csimflags="" blackbox="false"/>
    </files>
    <solutions>
        <solution name="solution1" status=""/>
    </solutions>
    <Simulation argv="">
        <SimFlow name="csim" setup="false" optimizeCompile="false" clean="false" ldflags="" mflags=""/>
    </Simulation>
</AutoPilot:project>

