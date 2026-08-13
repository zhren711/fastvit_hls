<AutoPilot:project xmlns:AutoPilot="com.autoesl.autopilot.project" projectType="C/C++" name="dwconv_ip_proj" ideType="classic" top="dwconv_ip">
    <files>
        <file name="dwconv_ip.h" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="dwconv_ip.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="../../tb_dwconv_ip.cpp" sc="0" tb="1" cflags="-std=c++14 -Wno-unknown-pragmas" csimflags="" blackbox="false"/>
    </files>
    <Simulation argv="">
        <SimFlow name="csim" setup="false" optimizeCompile="true" clean="true" ldflags="" mflags=""/>
    </Simulation>
    <solutions>
        <solution name="solution1" status=""/>
    </solutions>
</AutoPilot:project>

