#include <stdio.h>

#include <rockchip/rk_mpi.h>

int main(void)
{
    MPP_RET result =
        mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingAV1);

    printf("mpp_api_av1_decode=%s\n",
           result == MPP_OK ? "advertised" : "not-advertised");
    printf("mpp_api_result=%d\n", result);
    return 0;
}
