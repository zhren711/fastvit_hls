<AutoPilot:project xmlns:AutoPilot="com.autoesl.autopilot.project" projectType="C/C++" name="mac_array_poc" ideType="classic" top="mac_array_top">
    <files>
        <file name="mac_array.cpp" sc="0" tb="false" cflags="-std=c++14" csimflags="" blackbox="false"/>
        <file name="../../mac_array_tb.cpp" sc="0" tb="1" cflags="-std=c++14 -Wno-unknown-pragmas" csimflags="" blackbox="false"/>
    </files>
    <Simulation argv="">
        <SimFlow name="csim" setup="false" optimizeCompile="false" clean="false" ldflags="" mflags=""/>
    </Simulation>
    <solutions>
        <solution name="solution1" status=""/>
        <solution name="solution_export_rowhoist" status=""/>
        <solution name="solution_export_mulfix" status=""/>
        <solution name="solution_export_totalfield" status=""/>
        <solution name="solution_export_baseline_recheck" status=""/>
        <solution name="solution_export_dualfix" status=""/>
        <solution name="solution_export_macpd1" status=""/>
        <solution name="solution_macpd_probe2" status=""/>
        <solution name="solution_macpd_probe4" status=""/>
        <solution name="solution_macpd_probe8" status=""/>
        <solution name="solution_pwflat_iifix" status=""/>
        <solution name="solution_export_pwflat_iifix" status=""/>
    </solutions>
</AutoPilot:project>

