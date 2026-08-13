<AutoPilot:project xmlns:AutoPilot="com.autoesl.autopilot.project" top="dsconv_worker" name="dsconv_fused_proj" ideType="classic" projectType="C/C++">
    <files>
        <file name="dsconv_worker.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="../../tb_dsconv_worker.cpp" sc="0" tb="1" cflags="-std=c++14 -Wno-unknown-pragmas" csimflags="" blackbox="false"/>
    </files>
    <Simulation argv="">
        <SimFlow name="csim" setup="false" optimizeCompile="false" clean="false" ldflags="" mflags=""/>
    </Simulation>
    <solutions>
        <solution name="solution1" status=""/>
    </solutions>
</AutoPilot:project>

