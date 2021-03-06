! Copyright (c) 2018, NVIDIA CORPORATION.  All rights reserved.
!
! Licensed under the Apache License, Version 2.0 (the "License");
! you may not use this file except in compliance with the License.
! You may obtain a copy of the License at
!
!     http://www.apache.org/licenses/LICENSE-2.0
!
! Unless required by applicable law or agreed to in writing, software
! distributed under the License is distributed on an "AS IS" BASIS,
! WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
! See the License for the specific language governing permissions and
! limitations under the License.

!DEF: /main MainProgram
program main
 !DEF: /main/x POINTER ObjectEntity REAL(4)
 pointer :: x
 !REF: /main/x
 real x
 !DEF: /main/y EXTERNAL, POINTER ProcEntity REAL(4)
 pointer :: y
 !REF: /main/y
 procedure (type(real)) :: y
 !DEF: /main/z (implicit) ObjectEntity REAL(4)
 !REF: /main/y
 z = y()
end program
