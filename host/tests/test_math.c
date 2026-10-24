#include "altitude.h"
#include "check.h"
#include "zero_cal.h"

void test_altitude(void)
{
    float h = 123.0f;

    // Reproducible reference checks for the test protocol; expected values from Python (double).
    CHECK(altitude_relative_m(101325.0f, 101325.0f, &h));
    CHECK_NEAR(h, 0.0, 1e-6); // p == p0
    CHECK(altitude_relative_m(100129.4f, 101325.0f, &h));
    CHECK_NEAR(h, 100.0208, 0.01); // p < p0 -> positive
    CHECK(altitude_relative_m(101400.0f, 101325.0f, &h));
    CHECK_NEAR(h, -6.2424, 0.01); // p > p0 -> negative
    CHECK(altitude_relative_m(95000.0f, 101325.0f, &h));
    CHECK_NEAR(h, 540.4319, 0.05);

    // float resolution near p0: 1 Pa (~8 cm) must still be visible.
    CHECK(altitude_relative_m(101324.0f, 101325.0f, &h));
    CHECK(h > 0.05f && h < 0.12f);

    // The reference cases of docs/TEST_PROTOCOL.md, section 5.
    CHECK(altitude_relative_m(101205.0f, 101325.0f, &h));
    CHECK_NEAR(h, 9.996, 0.01);
    CHECK(altitude_relative_m(101445.0f, 101325.0f, &h));
    CHECK_NEAR(h, -9.986, 0.01);
    CHECK(altitude_relative_m(97770.0f, 97887.6f, &h));
    CHECK_NEAR(h, 10.140, 0.01);
    h = 42.0f;
    CHECK(!altitude_relative_m(NAN, 101325.0f, &h));
    CHECK(!altitude_relative_m(101325.0f, NAN, &h));
    CHECK(!altitude_relative_m(0.0f, 101325.0f, &h));
    CHECK(!altitude_relative_m(101325.0f, 0.0f, &h));
    CHECK(!altitude_relative_m(INFINITY, 101325.0f, &h));
    CHECK(!altitude_relative_m(29999.0f, 101325.0f, &h));
    CHECK(!altitude_relative_m(125001.0f, 101325.0f, &h));
    CHECK(!altitude_relative_m(101325.0f, 101325.0f, NULL));
    CHECK_NEAR(h, 42.0, 0); // untouched on failure
}

void test_zero_cal(void)
{
    zero_cal_t zc;
    float p0 = 0, spread = 0;
    zero_cal_init(&zc);

    CHECK_EQ_INT(zero_cal_feed(&zc, true, 100000.0f, 0, &p0, &spread), ZERO_CAL_NONE);

    // 20 Hz: finishes at 1 s, not at the 10th sample (0.45 s).
    CHECK(zero_cal_start(&zc, 1000));
    CHECK(!zero_cal_start(&zc, 1010)); // repeated click
    zero_cal_result_t r = ZERO_CAL_NONE;
    uint32_t t = 1000;
    int fed = 0;
    while (r == ZERO_CAL_NONE && fed < 100) {
        t += 50;
        float p = 100000.0f + ((fed % 2) ? 2.0f : -2.0f);
        r = zero_cal_feed(&zc, true, p, t, &p0, &spread);
        fed++;
    }
    CHECK_EQ_INT(r, ZERO_CAL_DONE);
    CHECK_EQ_INT(t, 2000);
    CHECK_EQ_INT(fed, 20);
    CHECK_NEAR(p0, 100000.0, 0.01);
    CHECK_NEAR(spread, 4.0, 1e-3);
    CHECK(!zero_cal_running(&zc));

    // Only 9 valid samples within 3 s -> error, not a mean of 9.
    CHECK(zero_cal_start(&zc, 0));
    r = ZERO_CAL_NONE;
    for (t = 100; t <= 3100 && r == ZERO_CAL_NONE; t += 100) {
        bool valid = t % 300 == 0 && t < 3000; // 300, 600 ... 2700
        r = zero_cal_feed(&zc, valid, 100000.0f, t, &p0, &spread);
    }
    CHECK_EQ_INT(r, ZERO_CAL_ERR_TIMEOUT);
    CHECK(!zero_cal_running(&zc));

    // Out-of-range values are not counted even with valid=true.
    CHECK(zero_cal_start(&zc, 0));
    for (t = 50; t <= 3000; t += 50) {
        CHECK_EQ_INT(zero_cal_feed(&zc, true, 0.0f, t, &p0, &spread), ZERO_CAL_NONE);
    }
    CHECK_EQ_INT(zero_cal_poll(&zc, 3001), ZERO_CAL_ERR_TIMEOUT);

    // Sensor went silent: timeout via poll without feed.
    CHECK(zero_cal_start(&zc, 500));
    CHECK_EQ_INT(zero_cal_poll(&zc, 3500), ZERO_CAL_NONE);
    CHECK_EQ_INT(zero_cal_poll(&zc, 3501), ZERO_CAL_ERR_TIMEOUT);
    CHECK(zero_cal_start(&zc, 3600)); // can restart after an error

    // ms counter wraparound.
    zero_cal_init(&zc);
    CHECK(zero_cal_start(&zc, 0xFFFFFF00u));
    r = ZERO_CAL_NONE;
    t = 0xFFFFFF00u;
    for (int i = 0; i < 40 && r == ZERO_CAL_NONE; i++) {
        t += 50;
        r = zero_cal_feed(&zc, true, 99000.0f, t, &p0, &spread);
    }
    CHECK_EQ_INT(r, ZERO_CAL_DONE);
    CHECK_NEAR(p0, 99000.0, 0.01);
}
