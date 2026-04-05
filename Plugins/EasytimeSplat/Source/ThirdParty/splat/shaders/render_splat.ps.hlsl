/*
  Copyright (c) 2025 Easytime Technology Co., Ltd. See LICENSE.md.
*/

/**
 * Required headers:
 * - constants.hlsl
 */

/**
 * Colors a single fragment of a splat.
 *
 * @param sig_div_sqrt_2 - The distance to the center of the splat, in 蟽's, over
 * sqrt(2).
 * @param in_color - The splat's base color.
 * @param out_color - Fragment color.
 */
void main(in half2 sig_div_sqrt_2 : DELTA_STD_DEVS,
          in nointerpolation half4 in_color : COLOR,
          out half4 out_color : SV_Target0) {
  /**
	 * Find the square of the distance from the center of the splat, in standard
	 * deviations (蟽):
	 *
	 * r     = sqrt((x - 渭_x)^2 + (y - 渭_y)^2)
	 * r_蟽   = sqrt(  螖_蟽x^2    +    螖_蟽y^2  )
	 * r_蟽^2 =        螖_蟽x^2    +    螖_蟽y^2
	 *
	 * This is equivalent to a dot product of 螖_蟽 with itself:
	 *
	 * r_蟽^2 =     (螖_蟽x, 螖_蟽y) 鈰?(螖_蟽x, 螖_蟽y)
	 *
	 * Note: This is r_蟽^2 / 2, as it moves a division from here to earlier.
	 */
  half sig_sq_div_2 = dot(sig_div_sqrt_2, sig_div_sqrt_2);

  /**
	 * The Gaussian function (as used here) is of the form:
	 *
	 * g(x, y) = exp(-1/2 * ((x - x_0)^2 / 蟽_x^2 + (y - y_0) / 蟽_x^2))
	 *
	 * @see https://en.wikipedia.org/wiki/Gaussian_function
	 *
	 * Given *螖_蟽*, a vector representing the distance from the center of the
	 * splat in standard deviations, we can rewrite the Gaussian function as:
	 *
	 * g(x, y) = exp(-1/2 * (螖_蟽x^2 + 螖_蟽y^2))
	 *
	 * Where:
	 *   螖_蟽x = (x - x_0) / 蟽_x
	 *   螖_蟽y = (y - y_0) / 蟽_y
	 *
	 * From here, we can again rewrite the Gaussian function to the following:
	 *
	 * g(x, y) = exp(-r_蟽^2 / 2)
	 *
	 * Where:
	 *   r_蟽^2 = 螖_蟽x^2 + 螖_蟽y^2
	 *
	 * Note: As the 1/2 is a constant, it is applied "early" in the vertex
	 * shader. As such, clipping to x蟽 standard deviations is done by clipping
	 * at x^2 / 2, and no 1/2 is multiplied in here.
	 */
  half alpha_gaussian = (sig_sq_div_2 < CUTOFF_RADIUS_SIGMA_SQUARED_OVER_2)
                            ? in_color.a / exp(sig_sq_div_2)
                            : 0;
  out_color = half4(in_color.rgb, alpha_gaussian);
}

