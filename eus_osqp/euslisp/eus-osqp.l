(defvar *libeusosqp* (load-foreign (format nil "~A/lib/libeus_osqp.so" (ros::resolve-ros-path "package://eus_osqp"))))

(defforeign _solve-osqp-qp
  *libeusosqp*
  "solve_osqp_qp"
  (:string ;; ret
   :string ;; eval-weight-matrix
   :string ;; eval-coeff-vector
   :string ;; inequality-matrix
   :string ;; inequality-min-vector
   :string ;; inequality-max-vector
   :integer ;; state_len
   :integer ;; inequality_len
   :integer ;; verbose
   :string ;; return status
   )
  :string)

(defun solve-osqp-qp
  (&key (inf-var 1e35)
        (initial-state)
        (eval-weight-matrix)
        (eval-coeff-vector)
        (state-dim (length eval-coeff-vector))
        (equality-matrix)
        (equality-vector)
        (state-min-vector) ;; not recommended. use inequality-matrix and inequality-min-vector.
        (state-max-vector) ;; not recommended. use inequality-matrix and inequality-min-vector.
        (inequality-matrix)
        (inequality-min-vector)
        (inequality-max-vector)
        (inequality-dim (if inequality-matrix (array-dimension inequality-matrix 0) 0))
        (print-level 0) ;; 0 or 1
        (debug t)
        (status (float-vector 0))
        (check-status t)
        (solve-qp-mode) ;; unused. just for compatibility with solve-qpoases-qp.
        )
  (if (and inequality-matrix (= (length inequality-min-vector) 0))
      (setq inequality-min-vector (fill (instantiate float-vector inequality-dim) (- inf-var))))
  (if (and inequality-matrix (= (length inequality-max-vector) 0))
      (setq inequality-max-vector (fill (instantiate float-vector inequality-dim) inf-var)))
  (let ((ret (instantiate float-vector state-dim))
        (inequality-matrix
         (cond
          ((and equality-vector inequality-matrix)
           (concatenate-matrix-column equality-matrix inequality-matrix))
          (equality-matrix equality-matrix)
          (t inequality-matrix)))
        (inequality-max-vector
         (cond
          ((and equality-vector inequality-max-vector)
           (concatenate float-vector equality-vector inequality-max-vector))
          (equality-vector equality-vector)
          (t inequality-max-vector)))
        (inequality-min-vector
         (cond
          ((and equality-vector inequality-min-vector)
           (concatenate float-vector equality-vector inequality-min-vector))
          (equality-vector equality-vector)
          (t inequality-min-vector))))
    (when (or state-min-vector state-max-vector)
      (unless state-min-vector
        (setq state-min-vector (fill (instantiate float-vector state-dim) (- inf-var))))
      (unless state-max-vector
        (setq state-max-vector (fill (instantiate float-vector state-dim) inf-var)))
      (cond
       (inequality-matrix
        (setq inequality-matrix
              (concatenate-matrix-column inequality-matrix (unit-matrix state-dim)))
        (setq inequality-min-vector
              (concatenate float-vector inequality-min-vector state-min-vector))
        (setq inequality-max-vector
              (concatenate float-vector inequality-max-vector state-max-vector))
        )
       (t
        (setq inequality-matrix (unit-matrix state-dim))
        (setq inequality-min-vector state-min-vector)
        (setq inequality-max-vector state-max-vector)
        ))
      )
    (when (and (= (length inequality-min-vector) 0) (= (length inequality-max-vector) 0))
      (setq inequality-matrix (make-matrix 1 state-dim))
      (setq inequality-min-vector (fill (instantiate float-vector 1) (- inf-var)))
      (setq inequality-max-vector (fill (instantiate float-vector 1) inf-var)))
    (unless eval-weight-matrix
      (setq eval-weight-matrix (make-matrix state-dim state-dim)))
    (_solve-osqp-qp
     ret
     (array-entity eval-weight-matrix)
     eval-coeff-vector
     (array-entity inequality-matrix)
     inequality-min-vector
     inequality-max-vector
     state-dim
     (array-dimension inequality-matrix 0)
     print-level
     status)
    ;; Check status and return value
    (let ((int-status (round (elt status 0))))
      ;; Check and print
      (if debug
          (unless (= int-status 1)
            (print
             (case int-status
               (1 :OSQP_SOLVED)
               (2 :OSQP_SOLVED_INACCURATE)
               (-2 :OSQP_MAX_ITER_REACHED)
               (-3 :OSQP_PRIMAL_INFEASIBLE)
               (3 :OSQP_PRIMAL_INFEASIBLE_INACCURATE)
               (-4 :OSQP_DUAL_INFEASIBLE)
               (4 :OSQP_DUAL_INFEASIBLE_INACCURATE)
               (-5 :OSQP_SIGINT)
               (-6 :OSQP_TIME_LIMIT_REACHED)
               (-10 :OSQP_UNSOLVED)
               (-7 :OSQP_NON_CVX)
               (-100 :OSQP_INITIALIZE_FAILED)
               ))))
      ;; Return value
      (cond
       ((not check-status) ret)
       ((= int-status 1) ret)
       (t nil))
      )))