"use client";

import { useEffect, useState } from "react";

export const useSupports = (supportCondition: string) => {
  const [checkResult, setCheckResult] = useState<boolean | undefined>();

  useEffect(() => {
    setCheckResult(CSS.supports(supportCondition));
  }, [supportCondition]);

  return checkResult;
};
