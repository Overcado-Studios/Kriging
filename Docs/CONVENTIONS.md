# Numerical conventions

This document is normative for the active core.

## Coordinates and values

The portable kernel is unit-agnostic. An Unreal caller should supply model-local coordinates in centimetres, after subtracting any large world or geospatial origin. All arithmetic in the solver and CPU evaluator is `double`.

`partialSill` is a partial sill. For bounded structures, total sill is:

\[
C(0)=c_0+\sum_k c_k.
\]

## Structure functions and direct range

The exported normalized structure functions use the direct range parameter `a`:

\[
g_{sph}(r)=\begin{cases}1.5r-0.5r^3&r\le1\\1&r>1\end{cases},
\quad g_{exp}(r)=1-e^{-r},
\quad g_{gau}(r)=1-e^{-r^2},
\]

where `r=h/a`.

For Matérn:

\[
g_{mat}(h)=1-\frac{2^{1-\nu}}{\Gamma(\nu)}
\left(\frac{\sqrt{2\nu}h}{a}\right)^\nu
K_\nu\left(\frac{\sqrt{2\nu}h}{a}\right).
\]

The special case `nu = 0.5` is exactly the exponential model. There is no hard branch from Matérn to Gaussian at `nu = 10`.

For this parameterization:

\[
\rho_\nu(h;a)\xrightarrow[\nu\to\infty]{}
\exp\left(-\frac{h^2}{2a^2}\right).
\]

Therefore the plugin Gaussian matches the limit after the range conversion:

\[
a_{Gaussian}=\sqrt{2}\,a_{Matern}.
\]

A same-range Matérn/Gaussian equality test is mathematically wrong. The shipped validator deliberately checks the corrected conversion.

For `x=sqrt(2 nu) h/a >= 60`, the Matérn correlation is below double-significant scale throughout the accepted order range and the structure returns its asymptote, 1.0. This prevents Bessel underflow from becoming a NaN.

The power structure is `g=(h/a)^alpha`, with `0<alpha<2`. It is unbounded and is rejected for simple kriging.

## Analytic CPU evaluation

The CPU core evaluates spherical, exponential, Gaussian, Matérn, and power functions analytically. It does not contain the previous fifth-root or dyadic lookup remap. Consequently the numerical validator compiles and calls the production C++ evaluator directly; it does not claim evidence from a Python restatement.

## Anisotropy

Each structure owns its anisotropy. In 2D:

\[
\Delta'=R_z(-azimuth)\Delta,
\quad h=\sqrt{\Delta_x'^2+(\Delta_y'/r_y)^2}.
\]

In 3D:

\[
\Delta'=R_z(-azimuth)R_y(-dip)R_x(-plunge)\Delta,
\]

followed by scaling the second and third axes by `1/ratioY` and `1/ratioZ`.

When all applicable ratios equal one, rotation is bypassed. The isotropic result is therefore bit-identical at every angle.

## Matrix form

Bounded structures use covariance form:

\[
A=\begin{bmatrix}C&F\\F^T&0\end{bmatrix}.
\]

The system is symmetric indefinite, so the active implementation uses partial-pivot LU rather than Cholesky.

Power structures use `C=-gamma` with the same positive constraint block and a `-gamma` query vector. This is algebraically equivalent to the standard semivariogram system with the sign of the Lagrange multipliers changed.

## Drift basis scaling

Polynomial drift coordinates are centered at the midpoint of the effective sample bounds and divided by half-extents, floored at one unit. Linear and quadratic terms therefore remain dimensionless and near order one even when coordinates have large offsets.

External drift is centered by its sample mean and scaled by its maximum absolute deviation. A constant external drift is rejected because it is linearly dependent on the intercept. Any sampling failure returns a descriptive build error naming the effective and original sample index.

## Nugget, measurement variance, and exactness

Exact nugget mode includes the nugget on the matrix diagonal and in a coincident query right-hand side. Filtered mode includes it on the diagonal but omits it from the query right-hand side. Measurement variance is always diagonal-only.

There is no exact-location return shortcut for kriging. Exactness is whatever the solved system produces. A stable system is first attempted with zero ridge. Only failed factorizations escalate through bounded ridge values. Gaussian and high-order Matérn models may raise the effective nugget to the documented conditioning floor; exact mode includes the same effective nugget in the right-hand side and remains algebraically exact when no measurement variance or ridge is present.

IDW has its own exact-location behavior because it is the explicit fallback method, not a kriging property test.

## Duplicate merging

Samples are sorted by X, Y, Z, then original index. Within `mergeRadius`, duplicate clusters use complete-link admission: a candidate joins only when it lies within the radius of every existing member. This caps each cluster's diameter and prevents a chain at 0.9-radius spacing from collapsing transitively.

Values are averaged. Independent measurement variances are combined as the sum divided by the square of the cluster count. The build report records merge count and maximum cluster diameter.

## Dual evaluation and variance

The global dual vector is solved once:

\[
d=A^{-1}z_{ext},\qquad \hat z(x)=b(x)^T d.
\]

Global value evaluation uses a fixed stack right-hand side. Global variance solves into a fixed stack workspace:

\[
\sigma^2=C(0)-b^T A^{-1}b.
\]

For the power branch, variance is `-b^T A^{-1}b` in the signed system. Every negative result caused by rounding is clamped to zero and atomically counted.

The allocation gate applies to non-degraded global kriging calls after warm-up. Local selection and IDW currently use temporary standard containers and are not claimed allocation-free.

## Local neighborhoods

The static balanced k-d tree uses squared Euclidean distance and deterministic tie-breaking by original index, then sample index. Local selection can sector-balance four planar quadrants or eight 3D octants. Factorization cache hashes are only bucket selectors; full sorted neighbor lists must compare equal before reuse.

The cache admits at most 4,096 systems. At capacity, new systems remain valid for the current query but are not inserted. The cache is never flushed wholesale during evaluation.

This core gate has no tiled grid cross-fade and reports no blended tiled variance.

## Value transforms

Log transform chooses `delta = 1 - minimum`, so every sample shifts positive. Measurement variance is mapped with the delta-method derivative `variance/(z+delta)^2`.

Normal score uses deterministic rank order with original index as the tie-breaker and AS241 for inverse normal quantiles. The full rank table is retained in model state. Supplied measurement variance under normal score is interpreted as already being in transformed-score units because no stable derivative exists at rank ties.

Variance returned from a transformed model remains in transformed space. It is not silently presented as variance of the back-transformed estimate.

Lognormal bias correction belongs to model settings. `Evaluate` and `EvaluateWithVariance` use the same corrected value; value-only evaluation performs the variance solve when correction requires it.

## Leave-one-out cross-validation

The inverse-diagonal fast path is eligible only for global, untransformed, non-power, non-degraded kriging with zero ridge and zero measurement variance. It requires every relevant inverse diagonal to be finite and strictly positive. No absolute value is used to manufacture a standard error.

For 60 samples or fewer, every eligible fast result is compared at runtime with a complete point-by-point rebuild. On mismatch, the brute-force result is returned with an explicit message. The test suite covers `n=20` and `n=60`, all supported drift methods, and both nugget modes.

Above 60 samples, synchronous brute-force rebuilding is refused. A fast result may still be returned, but `verifiedAgainstBruteForce` remains false and the report says so.

For transformed brute-force CV, unstandardized errors are reported in original value space. Standard errors and standardized residuals are computed in the fold's transformed space and are labelled accordingly.

## Determinism

Input sorting, k-d tree tie-breaking, local neighbor sorting, and all test seeds are deterministic. Identical global builds on the same architecture must produce byte-identical dual weights. GPU single-precision determinism is not relevant because no GPU path is active in this package.
