        subroutine iddr_svd(m,n,a,krank,u,v,s,ier,r)
c
c       constructs a rank-krank SVD  u diag(s) v^T  approximating a,
c       where u is an m x krank matrix whose columns are orthonormal,
c       v is an n x krank matrix whose columns are orthonormal,
c       and diag(s) is a diagonal krank x krank matrix whose entries
c       are all nonnegative. This routine combines a QR code
c       (which is based on plane/Householder reflections)
c       with the LAPACK routine dgesdd.
c
c       input:
c       m -- first dimension of a and u
c       n -- second dimension of a, and first dimension of v
c       a -- matrix to be SVD'd
c       krank -- desired rank of the approximation to a
c
c       output:
c       u -- left singular vectors of a corresponding
c            to the k greatest singular values of a
c       v -- right singular vectors of a corresponding
c            to the k greatest singular values of a
c       s -- k greatest singular values of a
c       ier -- 0 when the routine terminates successfully;
c              nonzero when the routine encounters an error
c
c       work:
c       r -- must be at least
c            (krank+2)*n+8*min(m,n)+15*krank**2+8*krank
c            real*8 elements long
c
c       _N.B._: This routine destroys a. Also, please beware that
c               the source code for this routine could be clearer.
c
        implicit none
        character*1 jobz
        integer m,n,k,krank,iftranspose,ldr,ldu,ldvt,lwork,
     1          info,j,ier,io
        real*8 a(m,n),u(m,krank),v(n*krank),s(krank),r(*)
c
c
        io = 8*min(m,n)
c
c
        ier = 0
c
c
c       Compute a pivoted QR decomposition of a.
c
        call iddr_qrpiv(m,n,a,krank,r,r(io+1))
c
c
c       Extract R from the QR decomposition.
c
        call idd_retriever(m,n,a,krank,r(io+1))
c
c
c       Rearrange R according to ind (which is stored in r).
c
        call idd_permuter(krank,r,krank,n,r(io+1))
c
c
c       Use LAPACK to SVD R,
c       storing the krank (krank x 1) left singular vectors
c       in r(io+krank*n+1 : io+krank*n+krank*krank).
c
        jobz = 'S'
        ldr = krank
        lwork = 2*(3*krank**2+n+4*krank**2+4*krank)
        ldu = krank
        ldvt = krank
c
        call dgesdd(jobz,krank,n,r(io+1),ldr,s,r(io+krank*n+1),ldu,
     1              v,ldvt,r(io+krank*n+krank*krank+1),lwork,r,info)
c
        if(info .ne. 0) then
          ier = info
          return
        endif
c
c
c       Multiply the U from R from the left by Q to obtain the U
c       for A.
c
        do k = 1,krank
c
          do j = 1,krank
            u(j,k) = r(io+krank*n+j+krank*(k-1))
          enddo ! j
c
          do j = krank+1,m
            u(j,k) = 0
          enddo ! j
c
        enddo ! k
c
        iftranspose = 0
        call idd_qmatmat(iftranspose,m,n,a,krank,krank,u,r)
c
c
c       Transpose v to obtain r.
c
        call idd_transer(krank,n,v,r)
c
c
c       Copy r into v.
c
        do k = 1,n*krank
          v(k) = r(k)
        enddo ! k
c
c
        return
        end
c
c
c
c
        subroutine iddp_svd(lw,eps,m,n,a,krank,iu,iv,is,w,ier)
c
c       constructs a rank-krank SVD  U Sigma V^T  approximating a
c       to precision eps, where U is an m x krank matrix whose
c       columns are orthonormal, V is an n x krank matrix whose
c       columns are orthonormal, and Sigma is a diagonal krank x krank
c       matrix whose entries are all nonnegative.
c       The entries of U are stored in w, starting at w(iu);
c       the entries of V are stored in w, starting at w(iv).
c       The diagonal entries of Sigma are stored in w,
c       starting at w(is). This routine combines a QR code
c       (which is based on plane/Householder reflections)
c       with the LAPACK routine dgesdd.
c
c       input:
c       lw -- maximum usable length of w (in real*8 elements)
c       eps -- precision to which the SVD approximates a
c       m -- first dimension of a and u
c       n -- second dimension of a, and first dimension of v
c       a -- matrix to be SVD'd
c
c       output:
c       krank -- rank of the approximation to a
c       iu -- index in w of the first entry of the matrix
c             of orthonormal left singular vectors of a
c       iv -- index in w of the first entry of the matrix
c             of orthonormal right singular vectors of a
c       is -- index in w of the first entry of the array
c             of singular values of a
c       w -- array containing the singular values and singular vectors
c            of a; w doubles as a work array, and so must be at least
c            (krank+1)*(m+2*n+9)+8*min(m,n)+15*krank**2
c            real*8 elements long, where krank is the rank
c            output by the present routine
c       ier -- 0 when the routine terminates successfully;
c              -1000 when lw is too small;
c              other nonzero values when dgesdd bombs
c
c       _N.B._: This routine destroys a. Also, please beware that
c               the source code for this routine could be clearer.
c               w must be at least
c               (krank+1)*(m+2*n+9)+8*min(m,n)+15*krank**2
c               real*8 elements long, where krank is the rank
c               output by the present routine.
c
        implicit none
        character*1 jobz
        integer m,n,k,krank,iftranspose,ldr,ldu,ldvt,lwork,
     1          info,j,ier,io,iu,iv,is,ivi,isi,lw,lu,lv,ls
        real*8 a(m,n),w(*),eps
c
c
        io = 8*min(m,n)
c
c
        ier = 0
c
c
c       Compute a pivoted QR decomposition of a.
c
        call iddp_qrpiv(eps,m,n,a,krank,w,w(io+1))
c
c
        if(krank .gt. 0) then
c
c
c         Extract R from the QR decomposition.
c
          call idd_retriever(m,n,a,krank,w(io+1))
c
c
c         Rearrange R according to ind (which is stored in w).
c
          call idd_permuter(krank,w,krank,n,w(io+1))
c
c
c         Use LAPACK to SVD R,
c         storing the krank (krank x 1) left singular vectors
c         in w(io+krank*n+1 : io+krank*n+krank*krank).
c
          jobz = 'S'
          ldr = krank
          lwork = 2*(3*krank**2+n+4*krank**2+4*krank)
          ldu = krank
          ldvt = krank
c
          ivi = io+krank*n+krank*krank+lwork+1
          lv = n*krank
c
          isi = ivi+lv
          ls = krank
c
          if(lw .lt. isi+ls+m*krank-1) then
            ier = -1000
            return
          endif
c
          call dgesdd(jobz,krank,n,w(io+1),ldr,w(isi),w(io+krank*n+1),
     1                ldu,w(ivi),ldvt,w(io+krank*n+krank*krank+1),
     2                lwork,w,info)
c
          if(info .ne. 0) then
            ier = info
            return
          endif
c
c
c         Transpose w(ivi:ivi+lv-1) to obtain V.
c
          iv = 1
          call idd_transer(krank,n,w(ivi),w(iv))
c
c
c         Copy w(isi:isi+ls-1) into w(is:is+ls-1).
c
          is = iv+lv
c
          do k = 1,ls
            w(is+k-1) = w(isi+k-1)
          enddo ! k
c
c
c         Multiply the U from R from the left by Q to obtain the U
c         for A.
c
          iu = is+ls
          lu = m*krank
c
          do k = 1,krank
c
            do j = 1,krank
              w(iu-1+j+krank*(k-1)) = w(io+krank*n+j+krank*(k-1))
            enddo ! j
c
          enddo ! k
c
          do k = krank,1,-1
c
            do j = m,krank+1,-1
              w(iu-1+j+m*(k-1)) = 0
            enddo ! j
c
            do j = krank,1,-1
              w(iu-1+j+m*(k-1)) = w(iu-1+j+krank*(k-1))
            enddo ! j
c
          enddo ! k
c
          iftranspose = 0
          call idd_qmatmat(iftranspose,m,n,a,krank,krank,w(iu),
     1                     w(iu+lu+1))
c
c
        endif ! krank .gt. 0
c
c
        return
        end
c
c
c
c
