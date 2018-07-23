if(NOT "${config}" MATCHES "[Dd][Ee][Bb]")
  return()
endif()
foreach(pdb ${pdbs})
  if(EXISTS "${pdb}")
    message(STATUS "PDB Exists: ${pdb}")
  else()
    message(SEND_ERROR "PDB MISSING:\n ${pdb}")
  endif()
endforeach()
