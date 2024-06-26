# The following has been generated automatically from src/core/validity/qgsabstractvaliditycheck.h
QgsValidityCheckResult.Warning = QgsValidityCheckResult.Type.Warning
QgsValidityCheckResult.Critical = QgsValidityCheckResult.Type.Critical
# monkey patching scoped based enum
QgsAbstractValidityCheck.TypeLayoutCheck = QgsAbstractValidityCheck.Type.LayoutCheck
QgsAbstractValidityCheck.Type.TypeLayoutCheck = QgsAbstractValidityCheck.Type.LayoutCheck
QgsAbstractValidityCheck.TypeLayoutCheck.is_monkey_patched = True
QgsAbstractValidityCheck.TypeLayoutCheck.__doc__ = "Print layout validity check, triggered on exporting a print layout"
QgsAbstractValidityCheck.TypeUserCheck = QgsAbstractValidityCheck.Type.UserCheck
QgsAbstractValidityCheck.Type.TypeUserCheck = QgsAbstractValidityCheck.Type.UserCheck
QgsAbstractValidityCheck.TypeUserCheck.is_monkey_patched = True
QgsAbstractValidityCheck.TypeUserCheck.__doc__ = "Starting point for custom user types"
QgsAbstractValidityCheck.Type.__doc__ = "Check types\n\n" + '* ``TypeLayoutCheck``: ' + QgsAbstractValidityCheck.Type.LayoutCheck.__doc__ + '\n' + '* ``TypeUserCheck``: ' + QgsAbstractValidityCheck.Type.UserCheck.__doc__
# --
