macroScript OpenNovaExportAse category:"OpenNova" tooltip:"Export Novalogic ASE"
(
    on execute do
    (
        python.Execute "from opennova_max import ui; ui.export_ase()"
    )
)
