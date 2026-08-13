<AutoPilot:project xmlns:AutoPilot="com.autoesl.autopilot.project" projectType="C/C++" name="add_ip_proj" ideType="classic" top="add_ip">
    <Simulation argv="">
        <SimFlow name="csim" setup="false" optimizeCompile="false" clean="true" ldflags="" mflags=""/>
    </Simulation>
    <files>
        <file name="../../tb_add_ip.cpp" sc="0" tb="1" cflags="-Wno-unknown-pragmas" csimflags="" blackbox="false"/>
        <file name="add_ip.h" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="add_ip.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
    </files>
    <solutions>
        <solution name="solution1" status=""/>
    </solutions>
</AutoPilot:project>

