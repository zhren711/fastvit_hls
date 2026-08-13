<AutoPilot:project xmlns:AutoPilot="com.autoesl.autopilot.project" projectType="C/C++" name="pool_ip_proj" ideType="classic" top="pool_ip">
    <files>
        <file name="pool_ip.h" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="pool_ip.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="../../pool_ip_tb.cpp" sc="0" tb="1" cflags="-std=c++14 -Wno-unknown-pragmas" csimflags="" blackbox="false"/>
    </files>
    <Simulation argv="">
        <SimFlow name="csim" setup="false" optimizeCompile="true" clean="true" ldflags="" mflags=""/>
    </Simulation>
    <solutions>
        <solution name="solution1" status=""/>
    </solutions>
</AutoPilot:project>

