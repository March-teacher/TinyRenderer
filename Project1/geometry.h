#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// geometry —— 渲染器使用的轻量数学库
//
// 本项目没有引入 glm / Eigen 之类的第三方库，而是用一个很小的 vec/mat 模板
// 覆盖渲染管线所需的基础运算：点积、叉积、矩阵乘法、转置、逆矩阵和齐次坐标转换。
//
// 这里的目标不是做一个通用数学库，而是让图形学概念能直接落到代码上：
//   vec3 表示模型/世界/观察空间中的点或方向；
//   vec4 表示裁剪空间中的齐次坐标；
//   mat<4,4> 表示 ModelView / Projection / Viewport 等变换矩阵；
//   mat<3,3> 常用于三个顶点的 varying 插值数据或切线空间标架。
//
// 注意：所有分量都用 double，代码更容易读，数值也更稳定；对这个教学渲染器来说，
// 可读性比极限性能更重要。
// ─────────────────────────────────────────────────────────────────────────────
#include <cmath>
#include <cassert>
#include <iostream>

// 通用 n 维向量。默认用 data[] 存储；下面会对 2/3/4 维做特化，
// 这样常用向量既能通过下标访问，也能写 v.x / v.y / v.z。
template<int n> struct vec {
    double data[n] = { 0 };
    double& operator[](const int i) { assert(i >= 0 && i < n); return data[i]; }
    double  operator[](const int i) const { assert(i >= 0 && i < n); return data[i]; }
};

// 点积：结果是一个标量。
// 在渲染中常用于：
//   1. 计算两个方向夹角的余弦，如 n·l 得到漫反射强度；
//   2. 矩阵乘向量时，一行矩阵与一个向量做点积。
template<int n> double operator*(const vec<n>& lhs, const vec<n>& rhs) {
    double ret = 0;                         // N.B. Do not ever, ever use such for loops! They are highly confusing.
    for (int i = n; i--; ret += lhs[i] * rhs[i]); // Here I used them as a tribute to old-school game programmers fighting for every CPU cycle.
    return ret;                             // Once upon a time reverse loops were faster than the normal ones, it is not the case anymore.
}

// 向量加减和数乘都是逐分量运算。写成运算符后，渲染代码里可以保留数学公式的样子，
// 例如 center = (vmin + vmax) * 0.5，比手动写三个分量更不容易出错。
template<int n> vec<n> operator+(const vec<n>& lhs, const vec<n>& rhs) {
    vec<n> ret = lhs;
    for (int i = n; i--; ret[i] += rhs[i]);
    return ret;
}

template<int n> vec<n> operator-(const vec<n>& lhs, const vec<n>& rhs) {
    vec<n> ret = lhs;
    for (int i = n; i--; ret[i] -= rhs[i]);
    return ret;
}

template<int n> vec<n> operator*(const vec<n>& lhs, const double& rhs) {
    vec<n> ret = lhs;
    for (int i = n; i--; ret[i] *= rhs);
    return ret;
}

template<int n> vec<n> operator*(const double& lhs, const vec<n>& rhs) {
    return rhs * lhs;
}

template<int n> vec<n> operator/(const vec<n>& lhs, const double& rhs) {
    vec<n> ret = lhs;
    for (int i = n; i--; ret[i] /= rhs);
    return ret;
}

template<int n> std::ostream& operator<<(std::ostream& out, const vec<n>& v) {
    for (int i = 0; i < n; i++) out << v[i] << " ";
    return out;
}

template<> struct vec<2> {
    double x = 0, y = 0;
    double& operator[](const int i) { assert(i >= 0 && i < 2); return i ? y : x; }
    double  operator[](const int i) const { assert(i >= 0 && i < 2); return i ? y : x; }
};

template<> struct vec<3> {
    double x = 0, y = 0, z = 0;
    double& operator[](const int i) { assert(i >= 0 && i < 3); return i ? (1 == i ? y : z) : x; }
    double  operator[](const int i) const { assert(i >= 0 && i < 3); return i ? (1 == i ? y : z) : x; }
};

template<> struct vec<4> {
    double x = 0, y = 0, z = 0, w = 0;
    double& operator[](const int i) { assert(i >= 0 && i < 4); return i < 2 ? (i ? y : x) : (2 == i ? z : w); }
    double  operator[](const int i) const { assert(i >= 0 && i < 4); return i < 2 ? (i ? y : x) : (2 == i ? z : w); }
    vec<2> xy()  const { return { x, y }; }
    vec<3> xyz() const { return { x, y, z }; }
};

typedef vec<2> vec2;
typedef vec<3> vec3;
typedef vec<4> vec4;

// 向量长度：sqrt(v·v)。
template<int n> double norm(const vec<n>& v) {
    return std::sqrt(v * v);
}

// 单位化。调用方要保证 v 不是零向量；遇到可能退化的数据时，代码通常会先判断 norm。
template<int n> vec<n> normalized(const vec<n>& v) {
    return v / norm(v);
}

// 三维叉积：返回同时垂直于 v1 和 v2 的向量。
// 常见用途是由三角形两条边求面法线，方向由右手定则决定。
inline vec3 cross(const vec3& v1, const vec3& v2) {
    return { v1.y * v2.z - v1.z * v2.y, v1.z * v2.x - v1.x * v2.z, v1.x * v2.y - v1.y * v2.x };
}

// ── 齐次坐标升降维 ────────────────────────────────────────────────────────
// 渲染管线里顶点要在 vec3（几何量）和 vec4（齐次坐标）之间来回切换：
// 点用 embed<4>(p, 1)（w=1，参与平移），方向/法线用 embed<4>(d, 0)（w=0，不受平移影响）。
// 这两个函数把这种转换收敛到一处，避免各处手写 {v.x, v.y, v.z, 1}。

// 升维：把低维向量放进高维向量的前几个分量，其余分量填 fill
template<int n1, int n2> vec<n1> embed(const vec<n2>& v, const double fill = 1) {
    static_assert(n1 >= n2, "embed 只能升维");
    vec<n1> ret;
    for (int i = n1; i--; ret[i] = (i < n2 ? v[i] : fill));
    return ret;
}

// 降维：只保留高维向量的前 n1 个分量（例如 vec4 → vec3 丢掉 w）
template<int n1, int n2> vec<n1> proj(const vec<n2>& v) {
    static_assert(n1 <= n2, "proj 只能降维");
    vec<n1> ret;
    for (int i = n1; i--; ret[i] = v[i]);
    return ret;
}

template<int n> struct dt;

// 行主序矩阵。rows[i][j] 表示第 i 行第 j 列。
// 这个结构很小，但足够覆盖渲染管线里的全部线性代数：
//   mat<4,4> 负责空间变换；
//   mat<3,3> / mat<2,3> 常用来按列保存三角形三个顶点的属性。
template<int nrows, int ncols> struct mat {
    vec<ncols> rows[nrows] = { {} };

    vec<ncols>& operator[] (const int idx) { assert(idx >= 0 && idx < nrows); return rows[idx]; }
    const vec<ncols>& operator[] (const int idx) const { assert(idx >= 0 && idx < nrows); return rows[idx]; }

    // 单位矩阵（主对角线为 1）。变换矩阵的初值几乎总是它。
    static mat<nrows, ncols> identity() {
        mat<nrows, ncols> ret;
        for (int i = nrows; i--; )
            for (int j = ncols; j--; ret[i][j] = (i == j));
        return ret;
    }

    // 取第 idx 列（按列存放三个顶点的属性时很常用：varying[0..2] 就是三个顶点）
    vec<nrows> col(const int idx) const {
        assert(idx >= 0 && idx < ncols);
        vec<nrows> ret;
        for (int i = nrows; i--; ret[i] = rows[i][idx]);
        return ret;
    }

    void set_col(const int idx, const vec<nrows>& v) {
        assert(idx >= 0 && idx < ncols);
        for (int i = nrows; i--; rows[i][idx] = v[i]);
    }

    double det() const {
        return dt<ncols>::det(*this);
    }

    // 代数余子式：删除指定行列后求子式行列式，再乘 (-1)^(row+col)。
    // 它是伴随矩阵求逆的基础。
    double cofactor(const int row, const int col) const {
        mat<nrows - 1, ncols - 1> submatrix;
        for (int i = nrows - 1; i--; )
            for (int j = ncols - 1; j--; submatrix[i][j] = rows[i + int(i >= row)][j + int(j >= col)]);
        return submatrix.det() * ((row + col) % 2 ? -1 : 1);
    }

    mat<nrows, ncols> invert_transpose() const {
        // 这里直接生成"逆矩阵的转置"，而不是先求逆再转置。
        // 对法线变换来说正好需要这个形式：normal' = inverse(transpose(M)) * normal。
        mat<nrows, ncols> adjugate_transpose; // transpose to ease determinant computation, check the last line
        for (int i = nrows; i--; )
            for (int j = ncols; j--; adjugate_transpose[i][j] = cofactor(i, j));
        return adjugate_transpose / (adjugate_transpose[0] * rows[0]);
    }

    mat<nrows, ncols> invert() const {
        return invert_transpose().transpose();
    }

    mat<ncols, nrows> transpose() const {
        mat<ncols, nrows> ret;
        for (int i = ncols; i--; )
            for (int j = nrows; j--; ret[i][j] = rows[j][i]);
        return ret;
    }
};

template<int nrows, int ncols> vec<ncols> operator*(const vec<nrows>& lhs, const mat<nrows, ncols>& rhs) {
    // 行向量乘矩阵。项目里用得不多，但保留它可以让矩阵运算更完整。
    return (mat<1, nrows>{{lhs}}*rhs)[0];
}

template<int nrows, int ncols> vec<nrows> operator*(const mat<nrows, ncols>& lhs, const vec<ncols>& rhs) {
    // 矩阵乘列向量：结果的第 i 个分量 = 第 i 行与向量做点积。
    // ModelView * position、Projection * position 都走这条路径。
    vec<nrows> ret;
    for (int i = nrows; i--; ret[i] = lhs[i] * rhs);
    return ret;
}

template<int R1, int C1, int C2>mat<R1, C2> operator*(const mat<R1, C1>& lhs, const mat<C1, C2>& rhs) {
    // 标准矩阵乘法：左矩阵的行 × 右矩阵的列。
    // 组合变换时尤其重要，例如 Viewport * Projection * ModelView。
    mat<R1, C2> result;
    for (int i = R1; i--; )
        for (int j = C2; j--; )
            for (int k = C1; k--; result[i][j] += lhs[i][k] * rhs[k][j]);
    return result;
}

template<int nrows, int ncols>mat<nrows, ncols> operator*(const mat<nrows, ncols>& lhs, const double& val) {
    mat<nrows, ncols> result;
    for (int i = nrows; i--; result[i] = lhs[i] * val);
    return result;
}

template<int nrows, int ncols>mat<nrows, ncols> operator/(const mat<nrows, ncols>& lhs, const double& val) {
    mat<nrows, ncols> result;
    for (int i = nrows; i--; result[i] = lhs[i] / val);
    return result;
}

template<int nrows, int ncols>mat<nrows, ncols> operator+(const mat<nrows, ncols>& lhs, const mat<nrows, ncols>& rhs) {
    mat<nrows, ncols> result;
    for (int i = nrows; i--; )
        for (int j = ncols; j--; result[i][j] = lhs[i][j] + rhs[i][j]);
    return result;
}

template<int nrows, int ncols>mat<nrows, ncols> operator-(const mat<nrows, ncols>& lhs, const mat<nrows, ncols>& rhs) {
    mat<nrows, ncols> result;
    for (int i = nrows; i--; )
        for (int j = ncols; j--; result[i][j] = lhs[i][j] - rhs[i][j]);
    return result;
}

template<int nrows, int ncols> std::ostream& operator<<(std::ostream& out, const mat<nrows, ncols>& m) {
    for (int i = 0; i < nrows; i++) out << m[i] << std::endl;
    return out;
}

template<int n> struct dt { // template metaprogramming to compute the determinant recursively
    // 递归按第一行展开行列式。4x4 规模很小，教学项目用这种写法足够清楚。
    static double det(const mat<n, n>& src) {
        double ret = 0;
        for (int i = n; i--; ret += src[0][i] * src.cofactor(0, i));
        return ret;
    }
};

template<> struct dt<1> {   // template specialization to stop the recursion
    // 1x1 行列式就是唯一的元素，这是递归展开的终点。
    static double det(const mat<1, 1>& src) {
        return src[0][0];
    }
};

