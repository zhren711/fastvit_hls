<AutoPilot:project xmlns:AutoPilot="com.autoesl.autopilot.project" projectType="C/C++" name="conv_ip_proj" ideType="classic" top="conv_ip">
    <files>
        <file name="conv_ip.h" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="conv_ip.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="../../conv_ip_tb.cpp" sc="0" tb="1" cflags="-std=c++14 -Wno-unknown-pragmas" csimflags="" blackbox="false"/>
    </files>
    <Simulation argv="">
        <SimFlow name="csim" setup="false" optimizeCompile="true" clean="true" ldflags="" mflags=""/>
    </Simulation>
    <solutions>
        <solution name="solution1" status=""/>
    </solutions>
</AutoPilot:project>

