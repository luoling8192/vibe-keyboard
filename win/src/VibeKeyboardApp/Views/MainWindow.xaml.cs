using System.Windows;
using System.Windows.Controls;
using VibeKeyboardApp.ViewModels;
using VibeKeyboardApp.Views;

namespace VibeKeyboardApp.Views
{
    public partial class MainWindow : Window
    {
        private readonly AppModel _model;

        public MainWindow()
        {
            InitializeComponent();
            _model = (AppModel)DataContext;
            NavList.SelectedIndex = 0;
            NavigateToPage(0);
        }

        private void Window_Loaded(object sender, RoutedEventArgs e)
        {
            _model.Start();
        }

        private void NavList_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (NavList.SelectedIndex >= 0)
                NavigateToPage(NavList.SelectedIndex);
        }

        private void NavigateToPage(int index)
        {
            UserControl page = index switch
            {
                0 => new DevicePage { DataContext = _model },
                1 => new ScreenPage { DataContext = _model },
                2 => new KeysPage { DataContext = _model },
                3 => new AudioPage { DataContext = _model },
                4 => new FirmwarePage { DataContext = _model },
                _ => new DevicePage { DataContext = _model }
            };
            ContentArea.Content = page;
        }
    }
}
