<AutoPilot:project xmlns:AutoPilot="com.autoesl.autopilot.project" projectType="C/C++" name="cosim_dw7_proj" ideType="classic" top="dwconv_ip">
    <files>
        <file name="../dwconv_ip.h" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="../dwconv_ip.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="../../tb_cosim_dw7.cpp" sc="0" tb="1" cflags="-std=c++14 -I../../../. -Wno-unknown-pragmas" csimflags="" blackbox="false"/>
    </files>
    <solutions>
        <solution name="solution1" status=""/>
    </solutions>
    <Simulation argv="">
        <SimFlow name="csim" setup="false" optimizeCompile="false" clean="false" ldflags="" mflags=""/>
    </Simulation>
</AutoPilot:project>

