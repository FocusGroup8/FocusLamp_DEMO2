/* Actual policy-header tests. Compile with -DCALIBRATION_POLICY_STANDALONE
 * for a host executable, or call arm_calibration_policy_test() from a harness.
 * No UART, ESP-IDF, FreeRTOS or physical device is used. */
#include "../../components/motion_controller/include/motion_calibration_policy.h"
#include <limits.h>

#define CHECK(value) do { if (!(value)) return __LINE__; } while (0)

int arm_calibration_policy_test(void)
{
    const int axes[5]={1,2,3,0,4};
    for (int id=1; id<=5; ++id) {
        motion_calibration_config_t config={.servo_id=id,.min_position=100,.max_position=200,
            .step=2,.time_ms=id==4 ? 0 : 1500,.speed=id==4 ? 100 : 0};
        int maximum=id==4 ? 3000 : 1000;
        CHECK(motion_calibration_axis(id)==axes[id-1]);
        CHECK(motion_calibration_config_valid(&config,0,maximum));
        int target=-123;
        CHECK(motion_calibration_target(&config,150,1,&target)==MOTION_CALIBRATION_TARGET_OK);
        CHECK(target==152);
        CHECK(motion_calibration_target(&config,150,-1,&target)==MOTION_CALIBRATION_TARGET_OK);
        CHECK(target==148);
        target=-123;
        CHECK(motion_calibration_target(&config,199,1,&target)==MOTION_CALIBRATION_TRIAL_BOUNDARY);
        CHECK(target==-123);
        CHECK(motion_calibration_target(&config,100,-1,&target)==MOTION_CALIBRATION_TRIAL_BOUNDARY);
        CHECK(target==-123);
        CHECK(motion_calibration_target(&config,99,1,&target)==MOTION_CALIBRATION_CURRENT_OUTSIDE);
        CHECK(motion_calibration_target(&config,201,-1,&target)==MOTION_CALIBRATION_CURRENT_OUTSIDE);
        CHECK(motion_calibration_target(&config,150,0,&target)==MOTION_CALIBRATION_BAD_STEP);
        CHECK(motion_calibration_target(&config,150,2,&target)==MOTION_CALIBRATION_BAD_STEP);
        CHECK(motion_calibration_target(&config,150,1,0)==MOTION_CALIBRATION_BAD_STEP);
        config.step=0; CHECK(!motion_calibration_config_valid(&config,0,maximum));
        config.step=101; CHECK(!motion_calibration_config_valid(&config,0,maximum));
        config.step=2; config.min_position=-1; CHECK(!motion_calibration_config_valid(&config,0,maximum));
        config.min_position=100; config.max_position=maximum+1;
        CHECK(!motion_calibration_config_valid(&config,0,maximum));
        config.max_position=100; CHECK(!motion_calibration_config_valid(&config,0,maximum));
        config.max_position=200;
        if (id==4) {
            config.speed=0; CHECK(!motion_calibration_config_valid(&config,0,maximum));
            config.speed=501; CHECK(!motion_calibration_config_valid(&config,0,maximum));
            config.speed=100; config.time_ms=1500; CHECK(!motion_calibration_config_valid(&config,0,maximum));
        } else {
            config.time_ms=0; CHECK(!motion_calibration_config_valid(&config,0,maximum));
            config.time_ms=499; CHECK(!motion_calibration_config_valid(&config,0,maximum));
            config.time_ms=10001; CHECK(!motion_calibration_config_valid(&config,0,maximum));
            config.time_ms=1500; config.speed=100; CHECK(!motion_calibration_config_valid(&config,0,maximum));
        }
    }
    CHECK(motion_calibration_axis(0)==-1 && motion_calibration_axis(6)==-1);
    CHECK(!motion_calibration_config_valid(0,0,1000));
    motion_calibration_config_t large={.min_position=0,.max_position=INT_MAX,.step=INT_MAX};
    int target=-123;
    CHECK(motion_calibration_target(&large,INT_MAX,1,&target)==MOTION_CALIBRATION_TRIAL_BOUNDARY);
    CHECK(target==-123); /* No signed-int wrap or narrowed motor command. */
    return 0;
}

#ifdef CALIBRATION_POLICY_STANDALONE
int main(void) { return arm_calibration_policy_test(); }
#endif
