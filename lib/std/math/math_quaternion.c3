module std::math::quaternion(<Real>);
import std::math::vector;
union Quaternion
{
	struct
	{
		Real i, j, k, l;
	}
	Real[<4>] v;
}

macro Quaternion identity() => { 0, 0, 0, 1 };
macro Quaternion Quaternion.add(Quaternion a, Quaternion b) => Quaternion { .v = a.v + b.v };
macro Quaternion Quaternion.add_each(Quaternion a, Real b) => Quaternion { .v = a.v + b };
macro Quaternion Quaternion.sub(Quaternion a, Quaternion b) => Quaternion { .v = a.v - b.v };
macro Quaternion Quaternion.sub_each(Quaternion a, Real b) => Quaternion { .v = a.v - b };
macro Quaternion Quaternion.scale(Quaternion a, Real s) => Quaternion { .v = a.v * s };
macro Quaternion Quaternion.normalize(Quaternion q) => { .v = q.v.normalize() };
macro Real Quaternion.length(Quaternion q) => q.v.length();
macro Quaternion Quaternion.lerp(Quaternion q1, Quaternion q2, Real amount) => { .v = q1.v.lerp(q2.v, amount) };
macro Matrix4f Quaternion.to_matrixf(Quaternion* q) => into_matrix(q, Matrix4f);
macro Matrix4 Quaternion.to_matrix(Quaternion* q) => into_matrix(q, Matrix4);
fn Quaternion Quaternion.nlerp(Quaternion q1, Quaternion q2, Real amount) => { .v = q1.v.lerp(q2.v, amount).normalize() };

fn Quaternion Quaternion.invert(q)
{
	Real length_sq = q.v.dot(q.v);
	if (length_sq <= 0) return q;
	Real inv_length = 1 / length_sq;
	return { q.v[0] * -inv_length, q.v[1] * -inv_length, q.v[2] * -inv_length, q.v[3] * inv_length };
}

fn Quaternion Quaternion.slerp(q1, Quaternion q2, Real amount)
{
	Quaternion result = {};

	Real[<4>] q2v = q2.v;
	Real cos_half_theta = q1.v.dot(q2v);

	if (cos_half_theta < 0)
	{
		q2v = -q2v;
		cos_half_theta = -cos_half_theta;
	}

	if (cos_half_theta >= 1) return q1;

	Real[<4>] q1v = q1.v;
	if (cos_half_theta > 0.95f) return { .v = q1v.lerp(q2v, amount) };

	Real half_theta = math::cos(cos_half_theta);
	Real sin_half_theta = math::sqrt(1 - cos_half_theta * cos_half_theta);
	if (math::abs(sin_half_theta) < 0.001f)
	{
		return { .v = (q1v + q2v) * 0.5f };
	}
	Real ratio_a = math::sin((1 - amount) * half_theta) / sin_half_theta;
	Real ratio_b = math::sin(amount * half_theta) / sin_half_theta;
	return { .v = q1v * ratio_a + q2v * ratio_b };
}

fn Quaternion Quaternion.mul(a, Quaternion b)
{
	return { a.i * b.l + a.l * b.i + a.j * b.k - a.k * b.j,
			 a.j * b.l + a.l * b.j + a.k * b.i - a.i * b.k,
			 a.k * b.l + a.l * b.k + a.i * b.j - a.j * b.i,
			 a.l * b.l - a.i * b.i - a.j * a.j - a.k * a.k };
}

macro into_matrix(Quaternion* q, $Type) @private
{
	$Type result = { .m = { [0] = 1, [5] = 1, [10] = 1, [15] = 1 } };
	Quaternion norm = q.normalize();

	var ii = norm.i*norm.i;
	var jj = norm.j*norm.j;
	var kk = norm.k*norm.k;
	var ik = norm.i*norm.k;
	var ij = norm.i*q.j;
	var jk = norm.j*norm.k;
	var li = norm.l*norm.i;
	var lj = norm.l*norm.j;
	var lk = norm.l*norm.k;

	result.m00 = 1 - 2*(jj + kk);
	result.m01 = 2*(ik + lk);
	result.m02 = 2*(ij - lj);

	result.m10 = 2*(ij - lk);
	result.m11 = 1 - 2*(ii + kk);
	result.m12 = 2*(jk + li);

	result.m20 = 2*(ik + lj);
	result.m21 = 2*(jk - li);
	result.m22 = 1 - 2*(ii + jj);

	return result;
}